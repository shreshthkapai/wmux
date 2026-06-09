[CmdletBinding()]
param(
  [string]$Wmux = "",

  [string]$BuildDir = "",

  [string]$Config = "Debug",

  [switch]$SkipBuild,

  [switch]$SkipUnitTests,

  [switch]$Quick,

  [ValidateRange(60, 604800)]
  [int]$SoakDurationSeconds = 3600,

  [ValidateRange(1, 10000)]
  [int]$SoakMaxCycles = 1000,

  [ValidateRange(1, 4096)]
  [int]$MemoryGrowthLimitMB = 128
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Wmux)) {
  $Wmux = Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"
}
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $PSScriptRoot "..\build-vs"
}

$script:RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$script:WmuxPath = Resolve-Path $Wmux
$script:BuildPath = Resolve-Path $BuildDir
$script:RunId = Get-Date -Format "yyyyMMdd-HHmmss"
$script:ArtifactsRoot = Join-Path $script:RepoRoot "artifacts\release-gate\$script:RunId"

function Invoke-Step {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [scriptblock]$Body
  )

  Write-Host ""
  Write-Host "gate: $Name"
  & $Body
}

function Invoke-Native {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,

    [string[]]$Arguments = @()
  )

  $output = & $FilePath @Arguments 2>&1
  $exitCode = $LASTEXITCODE
  $text = ($output | Out-String).TrimEnd()
  if ($text.Length -gt 0) {
    Write-Host $text
  }
  if ($exitCode -ne 0) {
    throw "$FilePath $($Arguments -join ' ') failed with exit code $exitCode"
  }
}

function Invoke-Wmux {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,

    [switch]$AllowFailure
  )

  $output = & $script:WmuxPath @Arguments 2>&1
  $exitCode = $LASTEXITCODE
  $text = ($output | Out-String).TrimEnd()
  if ($text.Length -gt 0) {
    Write-Host $text
  }
  if (-not $AllowFailure -and $exitCode -ne 0) {
    throw "wmux $($Arguments -join ' ') failed with exit code $exitCode"
  }

  [pscustomobject]@{
    ExitCode = $exitCode
    Output = $text
  }
}

function Get-DaemonProcess {
  Get-CimInstance Win32_Process -Filter "Name = 'wmux.exe'" |
    Where-Object { $_.CommandLine -match '--daemon' } |
    Select-Object -First 1
}

function Get-DescendantProcesses {
  param([int]$RootProcessId)

  $all = Get-CimInstance Win32_Process
  $pending = [System.Collections.Generic.Queue[int]]::new()
  $pending.Enqueue($RootProcessId)
  $descendants = @()

  while ($pending.Count -gt 0) {
    $parent = $pending.Dequeue()
    $children = $all | Where-Object { $_.ParentProcessId -eq $parent }
    foreach ($child in $children) {
      $descendants += $child
      $pending.Enqueue([int]$child.ProcessId)
    }
  }

  $descendants
}

function Assert-NoDaemonChildShells {
  $daemon = Get-DaemonProcess
  if ($null -eq $daemon) {
    return
  }

  $orphans = @(Get-DescendantProcesses -RootProcessId ([int]$daemon.ProcessId) |
    Where-Object { $_.Name -in @("cmd.exe", "powershell.exe", "pwsh.exe") })
  if ($orphans.Count -gt 0) {
    $formatted = $orphans | Select-Object Name, ProcessId, ParentProcessId, CommandLine |
      Format-Table -AutoSize | Out-String
    throw "release gate found daemon child shells after cleanup`n$formatted"
  }
}

function Stop-Daemon {
  try {
    [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  } catch {
  }
}

function Invoke-Script {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string[]]$Arguments = @()
  )

  $path = Join-Path $PSScriptRoot $Name
  Invoke-Native -FilePath "powershell" -Arguments (@(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $path
  ) + $Arguments)
}

function Assert-SoakMemoryBounded {
  param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
  )

  $samples = Get-ChildItem -Path $OutputDirectory -Recurse -Filter "resource-samples.csv" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($null -eq $samples) {
    throw "soak did not produce resource-samples.csv under $OutputDirectory"
  }

  $rows = @(Import-Csv $samples.FullName)
  if ($rows.Count -lt 2) {
    throw "soak produced fewer than two resource samples: $($samples.FullName)"
  }

  $first = [int64]$rows[0].private_memory_bytes
  $last = [int64]$rows[$rows.Count - 1].private_memory_bytes
  $growth = $last - $first
  $limit = [int64]$MemoryGrowthLimitMB * 1024 * 1024
  Write-Host "gate: soak private memory growth $growth bytes; limit $limit bytes"
  if ($growth -gt $limit) {
    throw "private memory grew by $growth bytes, above release gate limit $limit"
  }

  $lastChildCount = [int]$rows[$rows.Count - 1].child_processes
  if ($lastChildCount -ne 0) {
    throw "soak ended with $lastChildCount daemon child processes"
  }
}

New-Item -ItemType Directory -Force -Path $script:ArtifactsRoot | Out-Null

$stressArguments = @("-Wmux", $script:WmuxPath)
$soakArguments = @(
  "-Wmux", $script:WmuxPath,
  "-DurationSeconds", $SoakDurationSeconds,
  "-MaxCycles", $SoakMaxCycles,
  "-OutputDirectory", (Join-Path $script:ArtifactsRoot "soak")
)

if ($Quick) {
  $stressArguments += @(
    "-SessionIterations", "5",
    "-PaneIterations", "2",
    "-AttachLoops", "2",
    "-HighOutputLines", "40",
    "-ResizeIterations", "4",
    "-CopyPasteLoops", "2"
  )
  $soakArguments = @(
    "-Wmux", $script:WmuxPath,
    "-DurationSeconds", "10",
    "-MaxCycles", "1",
    "-SessionIterations", "3",
    "-PaneIterations", "1",
    "-AttachLoops", "1",
    "-HighOutputLines", "25",
    "-ResizeIterations", "3",
    "-CopyPasteLoops", "1",
    "-OutputDirectory", (Join-Path $script:ArtifactsRoot "soak")
  )
}

try {
  Invoke-Step "clean daemon start" {
    Stop-Daemon
  }

  if (-not $SkipBuild) {
    Invoke-Step "build" {
      Invoke-Native -FilePath "cmake" -Arguments @("--build", $script:BuildPath, "--config", $Config)
    }
  }

  if (-not $SkipUnitTests) {
    Invoke-Step "unit tests" {
      Invoke-Native -FilePath "ctest" -Arguments @(
        "--test-dir",
        $script:BuildPath,
        "-C",
        $Config,
        "--output-on-failure"
      )

      $testExe = Join-Path $script:BuildPath "$Config\wmux_tests.exe"
      if (Test-Path $testExe) {
        Invoke-Native -FilePath $testExe
      }
    }
  }

  Invoke-Step "attach lifecycle and terminal cleanup paths" {
    $iterations = if ($Quick) { "2" } else { "10" }
    Invoke-Script -Name "test-attach-lifecycle.ps1" -Arguments @("-Wmux", $script:WmuxPath, "-Iterations", $iterations)
  }

  Invoke-Step "detach/reattach persistence" {
    Invoke-Script -Name "test-detach-reattach.ps1" -Arguments @("-Wmux", $script:WmuxPath)
  }

  Invoke-Step "daemon recovery" {
    Invoke-Script -Name "test-daemon-recovery.ps1" -Arguments @("-Wmux", $script:WmuxPath)
  }

  Invoke-Step "process cleanup and orphan shell check" {
    $iterations = if ($Quick) { "5" } else { "50" }
    Invoke-Script -Name "test-process-cleanup.ps1" -Arguments @("-Exe", $script:WmuxPath, "-Iterations", $iterations)
    Assert-NoDaemonChildShells
  }

  Invoke-Step "resize stress" {
    $iterations = if ($Quick) { "4" } else { "80" }
    Invoke-Script -Name "test-resize-stress.ps1" -Arguments @("-Wmux", $script:WmuxPath, "-Iterations", $iterations)
  }

  Invoke-Step "render throughput" {
    $lines = if ($Quick) { "100" } else { "1200" }
    Invoke-Script -Name "test-render-throughput.ps1" -Arguments @("-Wmux", $script:WmuxPath, "-Lines", $lines)
  }

  Invoke-Step "stress suite" {
    Invoke-Script -Name "test-stress-suite.ps1" -Arguments $stressArguments
  }

  Invoke-Step "soak and resource bounds" {
    Invoke-Script -Name "test-soak.ps1" -Arguments $soakArguments
    Assert-SoakMemoryBounded -OutputDirectory (Join-Path $script:ArtifactsRoot "soak")
  }

  Invoke-Step "known limitations documented" {
    $doc = Join-Path $script:RepoRoot "docs\release-gate.md"
    if (-not (Test-Path $doc)) {
      throw "missing docs\release-gate.md"
    }
    $text = Get-Content $doc -Raw
    foreach ($required in @("Known Limitations", "Release Criteria", "Not Stable Until")) {
      if ($text -notmatch [regex]::Escape($required)) {
        throw "docs\release-gate.md is missing '$required'"
      }
    }
  }

  Invoke-Step "final daemon cleanup" {
    Stop-Daemon
    Assert-NoDaemonChildShells
  }

  Write-Host ""
  Write-Host "ok: release gate completed; artifacts: $script:ArtifactsRoot"
}
catch {
  Write-Host ""
  Write-Host "release gate failed: $($_.Exception.Message)"
  Write-Host "artifacts: $script:ArtifactsRoot"
  throw
}
finally {
  Stop-Daemon
}

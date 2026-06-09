[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(10, 604800)]
  [int]$DurationSeconds = 1800,

  [ValidateRange(1, 10000)]
  [int]$MaxCycles = 1000,

  [ValidateRange(1, 1000)]
  [int]$SessionIterations = 20,

  [ValidateRange(1, 200)]
  [int]$PaneIterations = 6,

  [ValidateRange(1, 200)]
  [int]$AttachLoops = 6,

  [ValidateRange(10, 10000)]
  [int]$HighOutputLines = 250,

  [ValidateRange(1, 1000)]
  [int]$ResizeIterations = 24,

  [ValidateRange(1, 100)]
  [int]$CopyPasteLoops = 3,

  [string]$OutputDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Wmux {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,

    [switch]$AllowFailure
  )

  $previousErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $output = & $script:WmuxPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
  }
  finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }

  $text = ($output | Out-String).TrimEnd()
  if (-not $AllowFailure -and $exitCode -ne 0) {
    throw "wmux $($Arguments -join ' ') failed with exit code $exitCode`n$text"
  }

  [pscustomobject]@{
    ExitCode = $exitCode
    Output = $text
  }
}

function Get-StatusField {
  param(
    [Parameter(Mandatory = $true)]
    [string]$StatusOutput,

    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Default = ""
  )

  $pattern = "(?m)^$([regex]::Escape($Name)):\s+(.+?)\s*$"
  if ($StatusOutput -match $pattern) {
    return $Matches[1]
  }

  return $Default
}

function Get-DaemonProcessInfo {
  $daemon = Get-CimInstance Win32_Process -Filter "Name = 'wmux.exe'" |
    Where-Object { $_.CommandLine -match '--daemon' } |
    Select-Object -First 1

  if ($null -eq $daemon) {
    return $null
  }

  $process = Get-Process -Id ([int]$daemon.ProcessId) -ErrorAction SilentlyContinue
  if ($null -eq $process) {
    return $null
  }

  [pscustomobject]@{
    ProcessId = [int]$daemon.ProcessId
    WorkingSetBytes = [int64]$process.WorkingSet64
    PrivateMemoryBytes = [int64]$process.PrivateMemorySize64
    CpuSeconds = [double]$process.CPU
    HandleCount = [int]$process.HandleCount
    ThreadCount = [int]$process.Threads.Count
  }
}

function Get-DaemonChildCount {
  param([int]$ProcessId)

  if ($ProcessId -le 0) {
    return 0
  }

  $all = Get-CimInstance Win32_Process
  $pending = [System.Collections.Generic.Queue[int]]::new()
  $pending.Enqueue($ProcessId)
  $count = 0

  while ($pending.Count -gt 0) {
    $parent = $pending.Dequeue()
    foreach ($child in ($all | Where-Object { $_.ParentProcessId -eq $parent })) {
      ++$count
      $pending.Enqueue([int]$child.ProcessId)
    }
  }

  $count
}

function Write-ResourceSample {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [int]$Cycle,

    [Parameter(Mandatory = $true)]
    [string]$Phase
  )

  $status = (Invoke-Wmux -Arguments @("server", "status") -AllowFailure).Output
  $daemon = Get-DaemonProcessInfo
  $pidValue = if ($null -eq $daemon) { 0 } else { $daemon.ProcessId }
  $childCount = if ($null -eq $daemon) { 0 } else { Get-DaemonChildCount -ProcessId $daemon.ProcessId }

  $sample = [pscustomobject]@{
    timestamp_utc = [DateTime]::UtcNow.ToString("o")
    cycle = $Cycle
    phase = $Phase
    daemon_pid = $pidValue
    working_set_bytes = if ($null -eq $daemon) { 0 } else { $daemon.WorkingSetBytes }
    private_memory_bytes = if ($null -eq $daemon) { 0 } else { $daemon.PrivateMemoryBytes }
    cpu_seconds = if ($null -eq $daemon) { 0 } else { $daemon.CpuSeconds }
    handle_count = if ($null -eq $daemon) { 0 } else { $daemon.HandleCount }
    thread_count = if ($null -eq $daemon) { 0 } else { $daemon.ThreadCount }
    child_processes = $childCount
    sessions = Get-StatusField -StatusOutput $status -Name "sessions" -Default "0"
    live_shells = Get-StatusField -StatusOutput $status -Name "live shells" -Default "0"
    attach_clients = Get-StatusField -StatusOutput $status -Name "attach clients" -Default "0"
    attach_workers = Get-StatusField -StatusOutput $status -Name "attach workers" -Default "0"
    render_frames = Get-StatusField -StatusOutput $status -Name "render frames" -Default "0"
    render_bytes = Get-StatusField -StatusOutput $status -Name "render bytes" -Default "0"
    output_bytes_dropped = Get-StatusField -StatusOutput $status -Name "output bytes dropped" -Default "0"
  }

  $sample | Export-Csv -LiteralPath $Path -Append -NoTypeInformation
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$stressScript = Join-Path $PSScriptRoot "test-stress-suite.ps1"
$resolvedOutputDirectory =
  if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $PSScriptRoot "..\artifacts\soak"
  } else {
    $OutputDirectory
  }
$runId = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
$runDirectory = Join-Path $resolvedOutputDirectory $runId
$samplesPath = Join-Path $runDirectory "resource-samples.csv"

New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "soak run: $runId"
Write-Host "output: $runDirectory"

[void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
$cycle = 0

try {
  Write-ResourceSample -Path $samplesPath -Cycle $cycle -Phase "before"

  while ([DateTime]::UtcNow -lt $deadline -and $cycle -lt $MaxCycles) {
    ++$cycle
    $cycleLog = Join-Path $runDirectory ("cycle-{0:D4}.log" -f $cycle)
    Write-Host "soak: cycle $cycle"
    Write-ResourceSample -Path $samplesPath -Cycle $cycle -Phase "pre-cycle"

    try {
      $output = & $stressScript `
        -Wmux $script:WmuxPath `
        -NoRestart `
        -KeepDaemonRunning `
        -SessionIterations $SessionIterations `
        -PaneIterations $PaneIterations `
        -AttachLoops $AttachLoops `
        -HighOutputLines $HighOutputLines `
        -ResizeIterations $ResizeIterations `
        -CopyPasteLoops $CopyPasteLoops 2>&1
      $output | Set-Content -LiteralPath $cycleLog -Encoding UTF8
    }
    catch {
      $_ | Out-String | Set-Content -LiteralPath $cycleLog -Encoding UTF8
      Write-ResourceSample -Path $samplesPath -Cycle $cycle -Phase "failed-cycle"
      throw "soak cycle $cycle failed; see $cycleLog"
    }

    Write-ResourceSample -Path $samplesPath -Cycle $cycle -Phase "post-cycle"
  }

  Write-ResourceSample -Path $samplesPath -Cycle $cycle -Phase "after"
  Write-Host "ok: soak completed $cycle cycles; samples: $samplesPath"
}
finally {
  [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
}

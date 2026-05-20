[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 1000)]
  [int]$Iterations = 20,

  [ValidateRange(100, 30000)]
  [int]$TimeoutMilliseconds = 5000,

  [ValidateRange(0, 10000)]
  [int]$SettleMilliseconds = 250
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Wmux {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments,

    [switch]$AllowFailure
  )

  $output = & $script:WmuxPath @Arguments 2>&1
  $exitCode = $LASTEXITCODE
  $text = ($output | Out-String).TrimEnd()

  if (-not $AllowFailure -and $exitCode -ne 0) {
    throw "wmux $($Arguments -join ' ') failed with exit code $exitCode`n$text"
  }

  [pscustomobject]@{
    ExitCode = $exitCode
    Output = $text
  }
}

function Get-WmuxDaemonProcess {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath
  )

  $fullPath = [System.IO.Path]::GetFullPath($ExecutablePath)

  Get-CimInstance Win32_Process -Filter "Name = 'wmux.exe'" |
    Where-Object {
      $_.ExecutablePath -and
      ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $fullPath) -and
      $_.CommandLine -and
      ($_.CommandLine -match '(^|\s)--daemon(\s|$)')
    } |
    Sort-Object ProcessId |
    Select-Object -First 1
}

function Get-ChildShellProcesses {
  param(
    [Parameter(Mandatory = $true)]
    [int]$ParentProcessId
  )

  $shellNames = @("powershell.exe", "pwsh.exe", "cmd.exe")

  @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $ParentProcessId" |
    Where-Object { $shellNames -contains $_.Name } |
    Select-Object ProcessId, ParentProcessId, Name, CommandLine)
}

function New-ProcessIdSet {
  param(
    [array]$Processes = @()
  )

  $ids = @()
  foreach ($process in $Processes) {
    $ids += [int]$process.ProcessId
  }

  $ids
}

function Wait-ForNoNewShellProcesses {
  param(
    [Parameter(Mandatory = $true)]
    [int]$ParentProcessId,

    [int[]]$BaselineProcessIds = @(),

    [Parameter(Mandatory = $true)]
    [int]$TimeoutMilliseconds
  )

  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)

  do {
    $current = @(Get-ChildShellProcesses -ParentProcessId $ParentProcessId |
      Where-Object { $BaselineProcessIds -notcontains [int]$_.ProcessId })

    if ($current.Count -eq 0) {
      return @()
    }

    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  $current
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path

Write-Host "wmux executable: $script:WmuxPath"
[void](Invoke-Wmux -Arguments @("server", "status"))

$daemon = Get-WmuxDaemonProcess -ExecutablePath $script:WmuxPath
if (-not $daemon) {
  throw "wmux daemon process was not found for $script:WmuxPath"
}

$daemonPid = [int]$daemon.ProcessId
Write-Host "daemon pid: $daemonPid"

$baselineShells = @(Get-ChildShellProcesses -ParentProcessId $daemonPid)
$baselineIds = @(New-ProcessIdSet -Processes $baselineShells)
Write-Host "baseline daemon-owned shells: $($baselineShells.Count)"

$prefix = "wmux_cleanup_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$createdSessions = [System.Collections.Generic.List[string]]::new()

try {
  for ($index = 1; $index -le $Iterations; $index++) {
    $sessionName = "{0}_{1:D3}" -f $prefix, $index
    Write-Host ("[{0}/{1}] create/kill {2}" -f $index, $Iterations, $sessionName)

    [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))
    $createdSessions.Add($sessionName)

    if ($SettleMilliseconds -gt 0) {
      Start-Sleep -Milliseconds $SettleMilliseconds
    }

    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName))
    [void]$createdSessions.Remove($sessionName)

    $leakedShells = @(Wait-ForNoNewShellProcesses `
        -ParentProcessId $daemonPid `
        -BaselineProcessIds $baselineIds `
        -TimeoutMilliseconds $TimeoutMilliseconds)

    if ($leakedShells.Count -ne 0) {
      $details = ($leakedShells | Format-Table ProcessId, Name, CommandLine -AutoSize | Out-String).TrimEnd()
      throw "new daemon-owned shell processes remained after killing $sessionName`n$details"
    }
  }
}
finally {
  foreach ($sessionName in @($createdSessions)) {
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName) -AllowFailure)
  }
}

$remainingShells = @(Wait-ForNoNewShellProcesses `
    -ParentProcessId $daemonPid `
    -BaselineProcessIds $baselineIds `
    -TimeoutMilliseconds $TimeoutMilliseconds)

if ($remainingShells.Count -ne 0) {
  $details = ($remainingShells | Format-Table ProcessId, Name, CommandLine -AutoSize | Out-String).TrimEnd()
  throw "new daemon-owned shell processes remained after cleanup`n$details"
}

Write-Host "ok: $Iterations create/kill cycles completed without leaked daemon-owned shell processes"

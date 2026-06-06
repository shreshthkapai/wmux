param(
  [string]$Exe = ".\build-vs\Debug\wmux.exe",
  [int]$Iterations = 10
)

$ErrorActionPreference = "Stop"

function Invoke-Wmux {
  param([string[]]$Arguments)

  $output = & $Exe @Arguments 2>&1
  $exitCode = $LASTEXITCODE
  if ($exitCode -ne 0) {
    throw "wmux $($Arguments -join ' ') failed with exit code $exitCode`n$output"
  }
  return $output
}

function Get-DaemonProcess {
  Get-CimInstance Win32_Process -Filter "Name = 'wmux.exe'" |
    Where-Object { $_.CommandLine -match '--daemon' } |
    Select-Object -First 1
}

function Get-DescendantProcesses {
  param([int]$RootProcessId)

  $all = Get-CimInstance Win32_Process
  $pending = New-Object System.Collections.Generic.Queue[int]
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

  return $descendants
}

try {
  & $Exe server stop --force *> $null
} catch {
}

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
for ($i = 0; $i -lt $Iterations; ++$i) {
  $name = "cleanup-$i"
  [void](Invoke-Wmux -Arguments @("new", "-s", $name))
  [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name))
}

$status = Invoke-Wmux -Arguments @("server", "status")
$statusText = $status -join [Environment]::NewLine
if ($statusText -notmatch '(?m)^sessions:\s+0\s*$') {
  throw "expected zero sessions after cleanup loop`n$statusText"
}
if ($statusText -notmatch '(?m)^live shells:\s+0\s*$') {
  throw "expected zero live shells after cleanup loop`n$statusText"
}
if ($statusText -notmatch '(?m)^attach workers:\s+0\s*$') {
  throw "expected zero attach workers after cleanup loop`n$statusText"
}

$daemon = Get-DaemonProcess
if ($null -ne $daemon) {
  $orphans = Get-DescendantProcesses -RootProcessId ([int]$daemon.ProcessId) |
    Where-Object { $_.Name -in @("cmd.exe", "powershell.exe", "pwsh.exe") }
  if ($orphans.Count -gt 0) {
    $formatted = $orphans | Select-Object Name, ProcessId, ParentProcessId, CommandLine |
      Format-Table -AutoSize | Out-String
    throw "found shell descendants after cleanup loop`n$formatted"
  }
}

Write-Host "ok: repeated create/kill left no live sessions, live shells, or daemon child shells"

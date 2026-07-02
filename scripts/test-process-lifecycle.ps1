[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 120)]
  [int]$TimeoutSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\wmux-script-helpers.ps1"

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
    [string]$Name
  )

  $pattern = "(?m)^$([regex]::Escape($Name)):\s+(.+?)\s*$"
  if ($StatusOutput -match $pattern) {
    return $Matches[1]
  }

  throw "could not parse '${Name}' from server status`n$StatusOutput"
}

function Wait-StatusField {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [string]$Value
  )

  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $status = (Invoke-Wmux -Arguments @("server", "status")).Output
    if ((Get-StatusField -StatusOutput $status -Name $Name) -eq $Value) {
      return $status
    }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  throw "timed out waiting for '${Name}: $Value'"
}

function Get-WmuxDaemonProcess {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath
  )

  $fullPath = [System.IO.Path]::GetFullPath($ExecutablePath)
  $processName = [System.IO.Path]::GetFileName($fullPath).Replace("'", "''")

  Get-CimInstance Win32_Process -Filter "Name = '$processName'" |
    Where-Object {
      $_.ExecutablePath -and
      ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $fullPath) -and
      $_.CommandLine -and
      ($_.CommandLine -match '(^|\s)--daemon(\s|$)')
    } |
    Sort-Object ProcessId |
    Select-Object -First 1
}

function Get-DescendantProcesses {
  param(
    [Parameter(Mandatory = $true)]
    [int]$RootProcessId
  )

  $all = @(Get-CimInstance Win32_Process)
  $pending = [System.Collections.Generic.Queue[int]]::new()
  $pending.Enqueue($RootProcessId)
  $descendants = @()

  while ($pending.Count -gt 0) {
    $parent = $pending.Dequeue()
    $children = @($all | Where-Object { [int]$_.ParentProcessId -eq $parent })
    foreach ($child in $children) {
      $descendants += $child
      $pending.Enqueue([int]$child.ProcessId)
    }
  }

  $descendants
}

function Get-DaemonChildShells {
  param(
    [Parameter(Mandatory = $true)]
    [int]$DaemonProcessId
  )

  @(Get-DescendantProcesses -RootProcessId $DaemonProcessId |
    Where-Object { $_.Name -in @("cmd.exe", "powershell.exe", "pwsh.exe") } |
    Select-Object ProcessId, ParentProcessId, Name, CommandLine)
}

function Wait-DaemonChildShellCount {
  param(
    [Parameter(Mandatory = $true)]
    [int]$DaemonProcessId,

    [Parameter(Mandatory = $true)]
    [int]$Expected
  )

  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $shells = @(Get-DaemonChildShells -DaemonProcessId $DaemonProcessId)
    if ($shells.Count -eq $Expected) {
      return $shells
    }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  $shells = @(Get-DaemonChildShells -DaemonProcessId $DaemonProcessId)
  $details = ($shells | Format-Table ProcessId, ParentProcessId, Name, CommandLine -AutoSize | Out-String).TrimEnd()
  throw "expected $Expected daemon child shells, found $($shells.Count)`n$details"
}

function Wait-ProcessIdsExit {
  param(
    [Parameter(Mandatory = $true)]
    [int[]]$ProcessIds
  )

  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $alive = @()
    foreach ($processId in $ProcessIds) {
      $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
      if ($null -ne $process) {
        $alive += $process
      }
    }

    if ($alive.Count -eq 0) {
      return
    }

    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  $formatted = $alive | Select-Object Id, ProcessName, Path | Format-Table -AutoSize | Out-String
  throw "processes did not exit after forced daemon stop`n$formatted"
}

function New-AttachFrame {
  param(
    [ValidateSet("Detach", "CommandMode")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Detach" { 2 }
    "CommandMode" { 6 }
  }

  $length = [uint32]$Payload.Length
  $frame = [byte[]]::new(7 + $Payload.Length)
  $frame[0] = [byte][char]"W"
  $frame[1] = [byte][char]"M"
  $frame[2] = [byte]$typeByte
  $frame[3] = [byte]($length -band 0xff)
  $frame[4] = [byte](($length -shr 8) -band 0xff)
  $frame[5] = [byte](($length -shr 16) -band 0xff)
  $frame[6] = [byte](($length -shr 24) -band 0xff)

  if ($Payload.Length -gt 0) {
    [Array]::Copy($Payload, 0, $frame, 7, $Payload.Length)
  }

  $frame
}

function Write-PipeBytes {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [byte[]]$Bytes
  )

  Write-WmuxScriptPipeBytes -Pipe $Pipe -Bytes $Bytes
}

function Write-AttachCommandMode {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$Command
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (
    New-AttachFrame -Type CommandMode -Payload ([Text.Encoding]::UTF8.GetBytes($Command))
  )
}

function Write-AttachDetach {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type Detach)
}

function Read-ResponseLine {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe
  )

  Read-WmuxScriptResponseLine -Pipe $Pipe -TimeoutSeconds $TimeoutSeconds
}

function Open-Attach {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SessionName
  )

  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    (Get-WmuxAttachPipeName),
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous)

  $pipe.Connect($TimeoutSeconds * 1000)
  $request = '{{"type":"AttachStart","session_name":"{0}","terminal_columns":120,"terminal_rows":30}}' -f $SessionName
  Write-PipeBytes -Pipe $pipe -Bytes ([Text.Encoding]::UTF8.GetBytes($request + "`n"))

  $response = Read-ResponseLine -Pipe $pipe | ConvertFrom-Json
  if (-not $response.ok) {
    $pipe.Dispose()
    throw "attach failed: $($response.message)"
  }

  $pipe
}

function Read-UntilMarker {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$Pattern,

    [Parameter(Mandatory = $true)]
    [string]$Description
  )

  $output = ""
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

  while ([DateTime]::UtcNow -lt $deadline) {
    $remainingSeconds = [int][Math]::Max(1, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
    $output += Read-WmuxAttachOutputText -Pipe $Pipe -TimeoutSeconds $remainingSeconds
    if ([regex]::IsMatch($output, $Pattern)) {
      return $output
    }

    if ($output.Length -gt 1048576) {
      $output = $output.Substring($output.Length - 524288)
    }
  }

  throw "timed out waiting for $Description`nCaptured output:`n$output"
}

function Wait-AttachDrain {
  [void](Wait-StatusField -Name "attach clients" -Value "0")
  [void](Wait-StatusField -Name "attach workers" -Value "0")
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL
$sessionName = "wmux_lifecycle_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$attach = $null

Write-Host "wmux executable: $script:WmuxPath"

try {
  $env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
  [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  [void](Invoke-Wmux -Arguments @("server", "status"))

  $daemon = Get-WmuxDaemonProcess -ExecutablePath $script:WmuxPath
  if (-not $daemon) {
    throw "wmux daemon process was not found for $script:WmuxPath"
  }

  $daemonPid = [int]$daemon.ProcessId
  Write-Host "daemon pid: $daemonPid"

  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))
  [void](Wait-DaemonChildShellCount -DaemonProcessId $daemonPid -Expected 1)

  $attach = Open-Attach -SessionName $sessionName
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial shell prompt")

  Write-Host "lifecycle: split pane and kill active pane"
  Write-AttachCommandMode -Pipe $attach -Command "split-window -h"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane" -Description "split-pane status")
  [void](Wait-DaemonChildShellCount -DaemonProcessId $daemonPid -Expected 2)
  Write-AttachCommandMode -Pipe $attach -Command "kill-pane"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: killed pane" -Description "kill-pane status")
  [void](Wait-DaemonChildShellCount -DaemonProcessId $daemonPid -Expected 1)

  Write-Host "lifecycle: create window with split panes and kill active window"
  Write-AttachCommandMode -Pipe $attach -Command "new-window -n scratch"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: created window 'scratch'" -Description "new-window status")
  [void](Wait-DaemonChildShellCount -DaemonProcessId $daemonPid -Expected 2)
  Write-AttachCommandMode -Pipe $attach -Command "split-window -v"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane" -Description "window split status")
  [void](Wait-DaemonChildShellCount -DaemonProcessId $daemonPid -Expected 3)
  Write-AttachCommandMode -Pipe $attach -Command "kill-window"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: killed window" -Description "kill-window status")
  [void](Wait-DaemonChildShellCount -DaemonProcessId $daemonPid -Expected 1)

  Write-Host "lifecycle: force stop terminates daemon and descendants"
  $oldProcessIds = @([int]$daemonPid)
  $oldProcessIds += @(Get-DescendantProcesses -RootProcessId $daemonPid | ForEach-Object { [int]$_.ProcessId })

  [void](Invoke-Wmux -Arguments @("server", "stop", "--force"))
  Wait-ProcessIdsExit -ProcessIds $oldProcessIds

  $status = (Invoke-Wmux -Arguments @("server", "status")).Output
  if ((Get-StatusField -StatusOutput $status -Name "sessions") -ne "0") {
    throw "expected zero sessions after force stop`n$status"
  }
  if ((Get-StatusField -StatusOutput $status -Name "live shells") -ne "0") {
    throw "expected zero live shells after force stop`n$status"
  }
  if ((Get-StatusField -StatusOutput $status -Name "attach workers") -ne "0") {
    throw "expected zero attach workers after force stop`n$status"
  }

  Write-Host "ok: pane kill, window kill, and force stop cleaned up daemon-owned processes"
}
finally {
  if ($null -ne $attach) {
    try {
      Write-AttachDetach -Pipe $attach
    } catch {
    }
    $attach.Dispose()
  }

  [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName) -AllowFailure)
  [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)

  if ($null -eq $previousDefaultShell) {
    Remove-Item Env:WMUX_DEFAULT_SHELL -ErrorAction SilentlyContinue
  } else {
    $env:WMUX_DEFAULT_SHELL = $previousDefaultShell
  }
}

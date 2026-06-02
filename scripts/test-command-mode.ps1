[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 60)]
  [int]$TimeoutSeconds = 10
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

function New-AttachFrame {
  param(
    [ValidateSet("Input", "Detach", "CommandMode")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Input" { 1 }
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

  $Pipe.Write($Bytes, 0, $Bytes.Length)
  $Pipe.Flush()
}

function Write-AttachInput {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (
    New-AttachFrame -Type Input -Payload ([Text.Encoding]::UTF8.GetBytes($Text))
  )
}

function Write-CommandMode {
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

function Connect-AttachPipe {
  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    "wmux-attach",
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)

  $pipe.Connect($TimeoutSeconds * 1000)
  $pipe
}

function Read-ResponseLine {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe
  )

  $bytes = [System.Collections.Generic.List[byte]]::new()
  $buffer = [byte[]]::new(1)
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

  while ([DateTime]::UtcNow -lt $deadline) {
    $readTask = $Pipe.ReadAsync($buffer, 0, 1)
    $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $readTask.Wait($remaining)) {
      break
    }

    if ($readTask.Result -le 0) {
      break
    }

    $bytes.Add($buffer[0])
    if ($buffer[0] -eq [byte][char]"`n") {
      return [Text.Encoding]::UTF8.GetString($bytes.ToArray())
    }
  }

  throw "timed out waiting for attach response"
}

function Open-Attach {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SessionName
  )

  $pipe = Connect-AttachPipe
  $request = '{{"type":"AttachSession","session_name":"{0}","terminal_columns":120,"terminal_rows":30}}' -f $SessionName
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
  $buffer = [byte[]]::new(4096)
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

  while ([DateTime]::UtcNow -lt $deadline) {
    $readTask = $Pipe.ReadAsync($buffer, 0, $buffer.Length)
    $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $readTask.Wait($remaining)) {
      break
    }

    $count = $readTask.Result
    if ($count -le 0) {
      break
    }

    $output += [Text.Encoding]::UTF8.GetString($buffer, 0, $count)
    if ([regex]::IsMatch($output, $Pattern)) {
      return $output
    }
  }

  throw "timed out waiting for $Description`nCaptured output:`n$output"
}

function Get-SessionCount {
  param(
    [Parameter(Mandatory = $true)]
    [string]$StatusOutput
  )

  if ($StatusOutput -match '(?m)^sessions:\s+(\d+)\s*$') {
    return [int]$Matches[1]
  }

  throw "could not parse session count from server status`n$StatusOutput"
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$sessionName = "wmux_phase10b_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$renamedSession = "$($sessionName)_renamed"
$marker = "WMUX_PHASE10B_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL
$attach = $null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
$status = Invoke-Wmux -Arguments @("server", "status")
$sessionCount = Get-SessionCount -StatusOutput $status.Output
if ($sessionCount -ne 0) {
  throw "command-mode test requires an empty daemon because it restarts wmux with WMUX_DEFAULT_SHELL=cmd.exe /D /Q"
}

[void](Invoke-Wmux -Arguments @("server", "stop"))
[void](Invoke-Wmux -Arguments @("server", "status"))

try {
  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))

  $attach = Open-Attach -SessionName $sessionName
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

  Write-Host "command mode: reject unknown command"
  Write-CommandMode -Pipe $attach -Command "bogus"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: unknown command 'bogus'" -Description "unknown command status")

  Write-Host "command mode: new-window -n logs"
  Write-CommandMode -Pipe $attach -Command "new-window -n logs"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: created window 'logs'" -Description "new-window status")
  $windows = Invoke-Wmux -Arguments @("list-windows", "-t", $sessionName)
  if ($windows.Output -notmatch "(?m): logs \*$") {
    throw "expected logs to be active after command-mode new-window`n$($windows.Output)"
  }

  Write-Host "command mode: rename-window agents"
  Write-CommandMode -Pipe $attach -Command "rename-window agents"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: renamed active window to 'agents'" -Description "rename-window status")
  $windows = Invoke-Wmux -Arguments @("list-windows", "-t", $sessionName)
  if ($windows.Output -notmatch "(?m): agents \*$") {
    throw "expected active window to be renamed to agents`n$($windows.Output)"
  }

  Write-Host "command mode: split-window -h"
  Write-CommandMode -Pipe $attach -Command "split-window -h"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane horizontal" -Description "split-window status")
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P2`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P2")) -Description "active pane after split")

  Write-Host "command mode: kill-pane"
  Write-CommandMode -Pipe $attach -Command "kill-pane"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: killed pane" -Description "kill-pane status")
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P1`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P1")) -Description "active pane after kill-pane")

  Write-Host "command mode: refuse last-pane kill"
  Write-CommandMode -Pipe $attach -Command "kill-pane"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: cannot kill the last pane in a window" -Description "last-pane refusal")

  Write-Host "command mode: kill-window"
  Write-CommandMode -Pipe $attach -Command "new-window -n scratch"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: created window 'scratch'" -Description "scratch window status")
  Write-CommandMode -Pipe $attach -Command "kill-window"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: killed window" -Description "kill-window status")
  $windows = Invoke-Wmux -Arguments @("list-windows", "-t", $sessionName)
  if ($windows.Output -match "(?m): scratch") {
    throw "expected scratch window to be removed after command-mode kill-window`n$($windows.Output)"
  }

  Write-Host "command mode: kill remaining non-last window"
  Write-CommandMode -Pipe $attach -Command "kill-window"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: killed window" -Description "second kill-window status")

  Write-Host "command mode: refuse last-window kill"
  Write-CommandMode -Pipe $attach -Command "kill-window"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: cannot kill the last window in a session" -Description "last-window refusal")

  Write-Host "command mode: rename-session"
  Write-CommandMode -Pipe $attach -Command "rename-session $renamedSession"
  [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: renamed session to '$renamedSession'" -Description "rename-session status")
  $sessions = Invoke-Wmux -Arguments @("ls")
  if ($sessions.Output -notmatch "(?m)^$([regex]::Escape($renamedSession))$") {
    throw "expected session to be renamed`n$($sessions.Output)"
  }

  Write-AttachDetach -Pipe $attach
  $attach.Dispose()
  $attach = $null

  Write-Host "ok: command mode dispatches existing safe commands"
}
finally {
  if ($attach -ne $null) {
    $attach.Dispose()
  }

  try {
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $renamedSession) -AllowFailure)
  } catch {
  }
  try {
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName) -AllowFailure)
  } catch {
  }
  try {
    [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  } catch {
  }
  if ($null -eq $previousDefaultShell) {
    Remove-Item Env:WMUX_DEFAULT_SHELL -ErrorAction SilentlyContinue
  } else {
    $env:WMUX_DEFAULT_SHELL = $previousDefaultShell
  }
}

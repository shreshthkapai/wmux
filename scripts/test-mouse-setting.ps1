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
    [ValidateSet("Detach")]
    [string]$Type
  )

  $typeByte = switch ($Type) {
    "Detach" { 2 }
  }

  [byte[]]@(
    [byte][char]"W",
    [byte][char]"M",
    [byte]$typeByte,
    0,
    0,
    0,
    0
  )
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

function Open-AttachResponse {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SessionName
  )

  $pipe = Connect-AttachPipe
  try {
    $request = '{{"type":"AttachStart","session_name":"{0}","terminal_columns":90,"terminal_rows":20}}' -f $SessionName
    Write-PipeBytes -Pipe $pipe -Bytes ([Text.Encoding]::UTF8.GetBytes($request + "`n"))

    $response = Read-ResponseLine -Pipe $pipe | ConvertFrom-Json
    if (-not $response.ok) {
      throw "attach failed: $($response.message)"
    }

    if (-not ($response.PSObject.Properties.Name -contains "mouse_enabled")) {
      throw "attach response did not include mouse_enabled"
    }

    Write-PipeBytes -Pipe $pipe -Bytes (New-AttachFrame -Type Detach)
    return [bool]$response.mouse_enabled
  }
  finally {
    $pipe.Dispose()
  }
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

function Get-MouseSetting {
  param(
    [Parameter(Mandatory = $true)]
    [string]$StatusOutput
  )

  if ($StatusOutput -match '(?m)^mouse:\s+(on|off)\s*$') {
    return $Matches[1]
  }

  throw "could not parse mouse setting from server status`n$StatusOutput"
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$sessionName = "wmux_phase11d_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
$status = Invoke-Wmux -Arguments @("server", "status")
$sessionCount = Get-SessionCount -StatusOutput $status.Output
if ($sessionCount -ne 0) {
  throw "mouse-setting test requires an empty daemon because it restarts wmux with WMUX_DEFAULT_SHELL=cmd.exe /D /Q"
}

[void](Invoke-Wmux -Arguments @("server", "stop"))
[void](Invoke-Wmux -Arguments @("server", "status"))

try {
  $status = Invoke-Wmux -Arguments @("server", "status")
  if ((Get-MouseSetting -StatusOutput $status.Output) -ne "off") {
    throw "expected default mouse setting to be off`n$($status.Output)"
  }

  [void](Invoke-Wmux -Arguments @("set", "-g", "mouse", "on"))
  $status = Invoke-Wmux -Arguments @("server", "status")
  if ((Get-MouseSetting -StatusOutput $status.Output) -ne "on") {
    throw "expected mouse setting to be on`n$($status.Output)"
  }

  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))
  if (-not (Open-AttachResponse -SessionName $sessionName)) {
    throw "expected attach response mouse_enabled=true"
  }

  [void](Invoke-Wmux -Arguments @("set", "-g", "mouse", "off"))
  $status = Invoke-Wmux -Arguments @("server", "status")
  if ((Get-MouseSetting -StatusOutput $status.Output) -ne "off") {
    throw "expected mouse setting to be off`n$($status.Output)"
  }

  if (Open-AttachResponse -SessionName $sessionName) {
    throw "expected attach response mouse_enabled=false"
  }

  Write-Host "ok: mouse setting persisted in daemon state and attach response"
}
finally {
  [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName) -AllowFailure)
  [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  if ($null -eq $previousDefaultShell) {
    Remove-Item Env:WMUX_DEFAULT_SHELL -ErrorAction SilentlyContinue
  } else {
    $env:WMUX_DEFAULT_SHELL = $previousDefaultShell
  }
}

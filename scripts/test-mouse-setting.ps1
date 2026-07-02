[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 60)]
  [int]$TimeoutSeconds = 10
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
  try {
    $ErrorActionPreference = "Continue"
    $output = & $script:WmuxPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String).TrimEnd()
  }
  finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }

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

  Write-WmuxScriptPipeBytes -Pipe $Pipe -Bytes $Bytes
}

function Connect-AttachPipe {
  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    (Get-WmuxAttachPipeName),
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous)

  $pipe.Connect($TimeoutSeconds * 1000)
  $pipe
}

function Read-ResponseLine {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe
  )

  Read-WmuxScriptResponseLine -Pipe $Pipe -TimeoutSeconds $TimeoutSeconds
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

function Get-EffectiveMouseSupport {
  $doctor = Invoke-Wmux -Arguments @("doctor", "--json")
  $json = $doctor.Output | ConvertFrom-Json
  if (-not ($json.PSObject.Properties.Name -contains "terminal_capabilities")) {
    throw "doctor JSON did not include terminal_capabilities"
  }
  return [bool]$json.terminal_capabilities.supports_sgr_mouse
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
  $expectedMouseWhenOn = Get-EffectiveMouseSupport

  [void](Invoke-Wmux -Arguments @("set", "-g", "mouse", "off"))
  $status = Invoke-Wmux -Arguments @("server", "status")
  if ((Get-MouseSetting -StatusOutput $status.Output) -ne "off") {
    throw "expected mouse setting to be off`n$($status.Output)"
  }

  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))
  if (Open-AttachResponse -SessionName $sessionName) {
    throw "expected attach response mouse_enabled=false when daemon mouse is off"
  }

  [void](Invoke-Wmux -Arguments @("set", "-g", "mouse", "on"))
  $status = Invoke-Wmux -Arguments @("server", "status")
  if ((Get-MouseSetting -StatusOutput $status.Output) -ne "on") {
    throw "expected mouse setting to be on`n$($status.Output)"
  }

  $attachMouseWhenOn = Open-AttachResponse -SessionName $sessionName
  if ($attachMouseWhenOn -ne $expectedMouseWhenOn) {
    throw "expected attach response mouse_enabled=$expectedMouseWhenOn when daemon mouse is on, got $attachMouseWhenOn"
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

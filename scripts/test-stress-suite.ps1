[CmdletBinding()]
param(
  [string]$Wmux = "",

  [ValidateRange(1, 120)]
  [int]$TimeoutSeconds = 20,

  [ValidateRange(1, 1000)]
  [int]$SessionIterations = 100,

  [ValidateRange(1, 200)]
  [int]$PaneIterations = 25,

  [ValidateRange(1, 200)]
  [int]$AttachLoops = 25,

  [ValidateRange(10, 10000)]
  [int]$HighOutputLines = 800,

  [ValidateRange(1, 1000)]
  [int]$ResizeIterations = 80,

  [ValidateRange(1, 100)]
  [int]$CopyPasteLoops = 8,

  [ValidateRange(1, 100)]
  [int]$WindowIterations = 8,

  [ValidateRange(1, 5000)]
  [int]$MouseEventIterations = 250,

  [ValidateRange(10, 10000)]
  [int]$UnicodeLines = 500,

  [ValidateRange(1, 100)]
  [int]$ShellSpawnFailureLoops = 3,

  [switch]$NoRestart,

  [switch]$KeepDaemonRunning,

  [ValidateSet(
    "All",
    "CreateKill",
    "AttachDetach",
    "WindowSwitch",
    "PaneSplitKill",
    "OutputResize",
    "CopyPaste",
    "MouseFlood",
    "UnicodeOutput",
    "ShellSpawnFailure",
    "DaemonIpc")]
  [string[]]$Only = @("All")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:ScriptRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($script:ScriptRoot)) {
  $scriptPath = $MyInvocation.MyCommand.Path
  if ([string]::IsNullOrWhiteSpace($scriptPath)) {
    $scriptPath = Get-Variable -Name PSCommandPath -ValueOnly -ErrorAction SilentlyContinue
  }
  if (-not [string]::IsNullOrWhiteSpace($scriptPath)) {
    $script:ScriptRoot = Split-Path -Parent $scriptPath
  }
}
if ([string]::IsNullOrWhiteSpace($script:ScriptRoot)) {
  throw "Unable to resolve script root for test-stress-suite.ps1"
}
if ([string]::IsNullOrWhiteSpace($Wmux)) {
  $Wmux = Join-Path $script:ScriptRoot "..\build-vs\Debug\wmux.exe"
}

. (Join-Path $script:ScriptRoot "wmux-script-helpers.ps1")

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

function Assert-StatusValue {
  param(
    [Parameter(Mandatory = $true)]
    [string]$StatusOutput,

    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [string]$Value
  )

  $actual = Get-StatusField -StatusOutput $StatusOutput -Name $Name
  if ($actual -ne $Value) {
    throw "expected '${Name}: $Value', got '${Name}: $actual'`n$StatusOutput"
  }
}

function New-AttachFrame {
  param(
    [ValidateSet(
      "Input",
      "Detach",
      "Command",
      "CommandMode",
      "Resize",
      "MouseFocus",
      "MouseEvent",
      "Scroll",
      "CopyMode",
      "Paste")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Input" { 1 }
    "Detach" { 2 }
    "Command" { 3 }
    "CommandMode" { 6 }
    "Resize" { 4 }
    "MouseFocus" { 7 }
    "MouseEvent" { 8 }
    "Scroll" { 9 }
    "CopyMode" { 10 }
    "Paste" { 11 }
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

function Write-AttachCommand {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$Command
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (
    New-AttachFrame -Type Command -Payload ([Text.Encoding]::UTF8.GetBytes($Command))
  )
}

function Write-AttachResize {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateRange(1, 512)]
    [int]$Columns,

    [ValidateRange(1, 256)]
    [int]$Rows
  )

  $payload = [byte[]]::new(4)
  $payload[0] = [byte]($Columns -band 0xff)
  $payload[1] = [byte](($Columns -shr 8) -band 0xff)
  $payload[2] = [byte]($Rows -band 0xff)
  $payload[3] = [byte](($Rows -shr 8) -band 0xff)
  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type Resize -Payload $payload)
}

function Write-AttachMouseFocus {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateRange(1, 32767)]
    [int]$Column,

    [ValidateRange(1, 32767)]
    [int]$Row
  )

  $payload = [byte[]]::new(4)
  $payload[0] = [byte]($Column -band 0xff)
  $payload[1] = [byte](($Column -shr 8) -band 0xff)
  $payload[2] = [byte]($Row -band 0xff)
  $payload[3] = [byte](($Row -shr 8) -band 0xff)
  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type MouseFocus -Payload $payload)
}

function Write-AttachMouseEvent {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateRange(1, 32767)]
    [int]$Column,

    [ValidateRange(1, 32767)]
    [int]$Row,

    [ValidateRange(0, 65535)]
    [int]$ButtonCode,

    [ValidateSet("Left", "Middle", "Right", "Release", "WheelUp", "WheelDown", "Other")]
    [string]$Button,

    [ValidateSet("Press", "Release", "Drag", "Wheel")]
    [string]$Action
  )

  $buttonByte = switch ($Button) {
    "Left" { 0 }
    "Middle" { 1 }
    "Right" { 2 }
    "Release" { 3 }
    "WheelUp" { 4 }
    "WheelDown" { 5 }
    "Other" { 6 }
  }

  $actionByte = switch ($Action) {
    "Press" { 0 }
    "Release" { 1 }
    "Drag" { 2 }
    "Wheel" { 3 }
  }

  $payload = [byte[]]::new(8)
  $payload[0] = [byte]($Column -band 0xff)
  $payload[1] = [byte](($Column -shr 8) -band 0xff)
  $payload[2] = [byte]($Row -band 0xff)
  $payload[3] = [byte](($Row -shr 8) -band 0xff)
  $payload[4] = [byte]($ButtonCode -band 0xff)
  $payload[5] = [byte](($ButtonCode -shr 8) -band 0xff)
  $payload[6] = [byte]$buttonByte
  $payload[7] = [byte]$actionByte
  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type MouseEvent -Payload $payload)
}

function Write-AttachScroll {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateSet("LineUp", "LineDown", "PageUp", "PageDown", "Bottom")]
    [string]$Action
  )

  $actionByte = switch ($Action) {
    "LineUp" { 0 }
    "LineDown" { 1 }
    "PageUp" { 2 }
    "PageDown" { 3 }
    "Bottom" { 4 }
  }

  Write-PipeBytes -Pipe $Pipe -Bytes (
    New-AttachFrame -Type Scroll -Payload ([byte[]]@([byte]$actionByte))
  )
}

function Write-AttachCopyMode {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [ValidateSet("Enter", "Exit", "CursorUp", "CursorDown", "CursorLeft", "CursorRight", "PageUp", "PageDown", "StartSelection", "CopySelection")]
    [string]$Action
  )

  $actionByte = switch ($Action) {
    "Enter" { 0 }
    "Exit" { 1 }
    "CursorUp" { 2 }
    "CursorDown" { 3 }
    "CursorLeft" { 4 }
    "CursorRight" { 5 }
    "PageUp" { 6 }
    "PageDown" { 7 }
    "StartSelection" { 8 }
    "CopySelection" { 9 }
  }

  Write-PipeBytes -Pipe $Pipe -Bytes (
    New-AttachFrame -Type CopyMode -Payload ([byte[]]@([byte]$actionByte))
  )
}

function Write-AttachPaste {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type Paste)
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

function Open-Attach {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SessionName,

    [int]$Columns = 120,

    [int]$Rows = 30
  )

  $pipe = Connect-AttachPipe
  $request = '{{"type":"AttachStart","session_name":"{0}","terminal_columns":{1},"terminal_rows":{2}}}' -f $SessionName, $Columns, $Rows
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
  $plainOutput = ""
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

  while ([DateTime]::UtcNow -lt $deadline) {
    $remainingSeconds = [int][Math]::Max(1, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
    try {
      $chunk = Read-WmuxAttachOutputText -Pipe $Pipe -TimeoutSeconds $remainingSeconds
      $output += $chunk
      $plainOutput += ConvertTo-PlainTerminalText -Text $chunk
    }
    catch {
      throw "timed out waiting for $Description`nLast read error: $($_.Exception.Message)`nCaptured output:`n$output`nPlain captured output:`n$plainOutput"
    }
    if ([regex]::IsMatch($output, $Pattern) -or [regex]::IsMatch($plainOutput, $Pattern)) {
      return $output
    }

    if ($output.Length -gt 1048576) {
      $output = $output.Substring($output.Length - 524288)
    }
    if ($plainOutput.Length -gt 1048576) {
      $plainOutput = $plainOutput.Substring($plainOutput.Length - 524288)
    }
  }

  throw "timed out waiting for $Description`nCaptured output:`n$output`nPlain captured output:`n$plainOutput"
}

function ConvertTo-PlainTerminalText {
  param(
    [AllowEmptyString()]
    [string]$Text
  )

  if ([string]::IsNullOrEmpty($Text)) {
    return ""
  }

  $esc = [char]27
  $plain = $Text
  $plain = [regex]::Replace($plain, "$esc\][^\a]*(\a|$esc\\)", "")
  $plain = [regex]::Replace($plain, "$esc\[[0-?]*[ -/]*[@-~]", "")
  $plain = [regex]::Replace($plain, "$esc.", "")
  $plain
}

function Drain-AttachOutput {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [int]$Milliseconds = 100
  )

  [void]$Pipe
  Start-Sleep -Milliseconds $Milliseconds
}

function Assert-CleanDaemonState {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  $status = $null
  while ([DateTime]::UtcNow -lt $deadline) {
    $status = Invoke-Wmux -Arguments @("server", "status")
    $sessions = Get-StatusField -StatusOutput $status.Output -Name "sessions"
    $shells = Get-StatusField -StatusOutput $status.Output -Name "live shells"
    $clients = Get-StatusField -StatusOutput $status.Output -Name "attach clients"
    $workers = Get-StatusField -StatusOutput $status.Output -Name "attach workers"
    if ($sessions -eq "0" -and $shells -eq "0" -and $clients -eq "0" -and $workers -eq "0") {
      return
    }
    Start-Sleep -Milliseconds 100
  }

  if ($null -eq $status) {
    $status = Invoke-Wmux -Arguments @("server", "status")
  }
  Assert-StatusValue -StatusOutput $status.Output -Name "sessions" -Value "0"
  Assert-StatusValue -StatusOutput $status.Output -Name "live shells" -Value "0"
  Assert-StatusValue -StatusOutput $status.Output -Name "attach clients" -Value "0"
  Assert-StatusValue -StatusOutput $status.Output -Name "attach workers" -Value "0"
}

function Wait-AttachDrain {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  while ([DateTime]::UtcNow -lt $deadline) {
    $status = Invoke-Wmux -Arguments @("server", "status")
    $clients = Get-StatusField -StatusOutput $status.Output -Name "attach clients"
    $workers = Get-StatusField -StatusOutput $status.Output -Name "attach workers"
    if ($clients -eq "0" -and $workers -eq "0") {
      return
    }
    Start-Sleep -Milliseconds 100
  }

  $finalStatus = Invoke-Wmux -Arguments @("server", "status")
  throw "timed out waiting for attach clients/workers to drain`n$($finalStatus.Output)"
}

function Invoke-InvalidCommandIpcProbe {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [byte[]]$Bytes,

    [string]$ExpectedPattern = '"ok":false',

    [switch]$NoResponseExpected
  )

  Write-Host "stress: invalid IPC probe: $Name"
  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    (Get-WmuxCommandPipeName),
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous)
  $pipe.Connect($TimeoutSeconds * 1000)
  try {
    $pipe.Write($Bytes, 0, $Bytes.Length)
    $pipe.Flush()

    if ($NoResponseExpected) {
      $pipe.Dispose()
      return
    }

    try {
      $frame = Read-WmuxIpcFrame -Pipe $pipe -TimeoutSeconds $TimeoutSeconds
    }
    catch {
      throw "invalid IPC probe '$Name' failed while waiting for response: $($_.Exception.Message)"
    }
    if ($frame.Kind -ne "Control" -and $frame.Kind -ne "Error") {
      throw "expected IPC response frame for $Name, saw $($frame.Kind)"
    }

    $response = [Text.Encoding]::UTF8.GetString($frame.Payload)
    if ($response -notmatch $ExpectedPattern) {
      throw "unexpected malformed IPC response for $Name`: $response"
    }
  }
  finally {
    $pipe.Dispose()
  }
}

function New-BadCommandIpcHeader {
  param(
    [string]$Magic = "WMUX",
    [uint16]$Version = 1,
    [byte]$Kind = 1,
    [uint64]$RequestId = 1,
    [uint32]$PayloadLength = 0
  )

  $bytes = [byte[]]::new(19)
  $magicBytes = [Text.Encoding]::ASCII.GetBytes($Magic)
  [Array]::Copy($magicBytes, 0, $bytes, 0, [Math]::Min(4, $magicBytes.Length))
  [Array]::Copy([BitConverter]::GetBytes($Version), 0, $bytes, 4, 2)
  $bytes[6] = $Kind
  [Array]::Copy([BitConverter]::GetBytes($RequestId), 0, $bytes, 7, 8)
  [Array]::Copy([BitConverter]::GetBytes($PayloadLength), 0, $bytes, 15, 4)
  $bytes
}

function Invoke-InvalidCommandIpcProbes {
  Write-Host "stress: invalid IPC probes"

  $randomBytes = [byte[]]::new(19)
  $rng = [System.Random]::new(1337)
  $rng.NextBytes($randomBytes)
  Invoke-InvalidCommandIpcProbe `
    -Name "random bytes" `
    -Bytes $randomBytes `
    -ExpectedPattern 'bad IPC frame magic|malformed IPC frame'

  Invoke-InvalidCommandIpcProbe `
    -Name "wrong magic" `
    -Bytes (New-BadCommandIpcHeader -Magic "BAD!" -Kind 1 -RequestId 100) `
    -ExpectedPattern 'bad IPC frame magic|malformed IPC frame'

  Invoke-InvalidCommandIpcProbe `
    -Name "wrong version" `
    -Bytes (New-BadCommandIpcHeader -Version 999 -Kind 1 -RequestId 101) `
    -ExpectedPattern 'unsupported IPC protocol version'

  Invoke-InvalidCommandIpcProbe `
    -Name "unknown kind" `
    -Bytes (New-BadCommandIpcHeader -Kind 99 -RequestId 102) `
    -ExpectedPattern 'unknown IPC frame kind'

  Invoke-InvalidCommandIpcProbe `
    -Name "huge length" `
    -Bytes (New-BadCommandIpcHeader -Kind 1 -RequestId 103 -PayloadLength ([uint32](4 * 1024 * 1024 + 1))) `
    -ExpectedPattern 'IPC frame payload is too large'

  Invoke-InvalidCommandIpcProbe `
    -Name "partial frame" `
    -Bytes ([byte[]](87, 77, 85)) `
    -NoResponseExpected

  Invoke-InvalidCommandIpcProbe `
    -Name "truncated json" `
    -Bytes (New-WmuxIpcFrame -Kind Control -RequestId 104 -Payload ([Text.Encoding]::UTF8.GetBytes("{"))) `
    -ExpectedPattern 'malformed daemon request'

  Invoke-InvalidCommandIpcProbe `
    -Name "unknown command" `
    -Bytes (New-WmuxIpcFrame -Kind Control -RequestId 105 -Payload ([Text.Encoding]::UTF8.GetBytes('{"type":"Bogus"}'))) `
    -ExpectedPattern 'daemon does not understand request'

  Invoke-InvalidCommandIpcProbe `
    -Name "invalid target" `
    -Bytes (New-WmuxIpcFrame -Kind Control -RequestId 106 -Payload ([Text.Encoding]::UTF8.GetBytes('{"type":"KillSession","session_name":"wmux_missing_target"}'))) `
    -ExpectedPattern 'session not found|not found|no such session'

  $invalidUtf8Payload = [byte[]](123, 34, 116, 121, 112, 101, 34, 58, 34, 255, 34, 125)
  Invoke-InvalidCommandIpcProbe `
    -Name "invalid utf8" `
    -Bytes (New-WmuxIpcFrame -Kind Control -RequestId 107 -Payload $invalidUtf8Payload) `
    -ExpectedPattern '"ok":false'

  $nested = '{"type":"Bogus","nested":' + ('[' * 128) + ('0' + (']' * 128)) + '}'
  Invoke-InvalidCommandIpcProbe `
    -Name "nested payload" `
    -Bytes (New-WmuxIpcFrame -Kind Control -RequestId 108 -Payload ([Text.Encoding]::UTF8.GetBytes($nested))) `
    -ExpectedPattern 'daemon does not understand request'
}

function Test-CreateKillSessions {
  Write-Host "stress: create/kill $SessionIterations sessions"
  for ($index = 0; $index -lt $SessionIterations; ++$index) {
    $name = "wmux_stress_session_$($script:RunId)_$index"
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name))
  }
  Assert-CleanDaemonState
}

function Test-AttachDetachLoops {
  Write-Host "stress: attach/detach $AttachLoops loops"
  $name = "wmux_stress_attach_$script:RunId"
  $attach = $null
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    for ($index = 0; $index -lt $AttachLoops; ++$index) {
      $attach = Open-Attach -SessionName $name
      [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "attached prompt")
      Write-AttachDetach -Pipe $attach
      $attach.Dispose()
      $attach = $null
      Wait-AttachDrain
    }
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-WindowSwitchingLoops {
  Write-Host "stress: create/switch $WindowIterations windows"
  $name = "wmux_stress_windows_$script:RunId"
  $marker = "WMUX_STRESS_WINDOWS_$script:RunId"
  $attach = $null
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    $attach = Open-Attach -SessionName $name
    [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

    Write-AttachInput -Pipe $attach -Text "echo $($marker)_W0`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_W0")) -Description "window 0 marker")

    for ($index = 1; $index -le $WindowIterations; ++$index) {
      Write-AttachCommand -Pipe $attach -Command "new-window"
      [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "new window prompt")
      Write-AttachInput -Pipe $attach -Text "echo $($marker)_W$index`r"
      [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_W$index")) -Description "new window marker")
    }

    for ($index = 0; $index -lt $WindowIterations; ++$index) {
      Write-AttachCommand -Pipe $attach -Command "previous-window"
      Drain-AttachOutput -Pipe $attach -Milliseconds 35
    }

    Write-AttachInput -Pipe $attach -Text "echo $($marker)_AFTER_PREV`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_AFTER_PREV")) -Description "previous-window routed input")

    for ($index = 0; $index -lt $WindowIterations; ++$index) {
      Write-AttachCommand -Pipe $attach -Command "next-window"
      Drain-AttachOutput -Pipe $attach -Milliseconds 35
    }

    Write-AttachInput -Pipe $attach -Text "echo $($marker)_AFTER_NEXT`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_AFTER_NEXT")) -Description "next-window routed input")

    $windows = Invoke-Wmux -Arguments @("list-windows", "-t", $name)
    $windowCount = @($windows.Output -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }).Count
    if ($windowCount -ne ($WindowIterations + 1)) {
      throw "expected $($WindowIterations + 1) windows, found $windowCount`n$($windows.Output)"
    }

    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-PaneSplitKillLoops {
  Write-Host "stress: split/kill panes $PaneIterations loops"
  $name = "wmux_stress_panes_$script:RunId"
  $marker = "WMUX_STRESS_PANES_$script:RunId"
  $attach = $null
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    $attach = Open-Attach -SessionName $name
    [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

    for ($index = 0; $index -lt $PaneIterations; ++$index) {
      $split = if (($index % 2) -eq 0) { "split-window -h" } else { "split-window -v" }
      Write-AttachCommandMode -Pipe $attach -Command $split
      [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane" -Description "split status")
      Write-AttachInput -Pipe $attach -Text "echo $($marker)_$index`r"
      [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_$index")) -Description "new pane shell response")
      Write-AttachCommandMode -Pipe $attach -Command "kill-pane"
      [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: killed pane" -Description "kill-pane status")
    }

    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-MouseEventFlood {
  Write-Host "stress: mouse event flood $MouseEventIterations events"
  $name = "wmux_stress_mouse_$script:RunId"
  $marker = "WMUX_STRESS_MOUSE_$script:RunId"
  $attach = $null
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    $attach = Open-Attach -SessionName $name -Columns 100 -Rows 30
    [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

    Write-AttachCommandMode -Pipe $attach -Command "split-window -h"
    [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane" -Description "horizontal split status")
    Write-AttachCommandMode -Pipe $attach -Command "split-window -v"
    [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane" -Description "vertical split status")

    $positions = @(
      @{ Column = 2; Row = 2 },
      @{ Column = 48; Row = 2 },
      @{ Column = 75; Row = 16 },
      @{ Column = 90; Row = 26 }
    )

    for ($index = 0; $index -lt $MouseEventIterations; ++$index) {
      $position = $positions[$index % $positions.Count]
      switch ($index % 6) {
        0 {
          Write-AttachMouseFocus -Pipe $attach -Column $position.Column -Row $position.Row
        }
        1 {
          Write-AttachMouseEvent `
            -Pipe $attach `
            -Column $position.Column `
            -Row $position.Row `
            -ButtonCode 0 `
            -Button Left `
            -Action Press
        }
        2 {
          Write-AttachMouseEvent `
            -Pipe $attach `
            -Column $position.Column `
            -Row $position.Row `
            -ButtonCode 0 `
            -Button Release `
            -Action Release
        }
        3 {
          Write-AttachMouseEvent `
            -Pipe $attach `
            -Column $position.Column `
            -Row $position.Row `
            -ButtonCode 64 `
            -Button WheelUp `
            -Action Wheel
        }
        4 {
          Write-AttachMouseEvent `
            -Pipe $attach `
            -Column $position.Column `
            -Row $position.Row `
            -ButtonCode 65 `
            -Button WheelDown `
            -Action Wheel
        }
        default {
          Write-AttachScroll -Pipe $attach -Action LineDown
        }
      }

      if (($index % 25) -eq 0) {
        Drain-AttachOutput -Pipe $attach -Milliseconds 20
      }
    }

    Write-AttachInput -Pipe $attach -Text "echo $($marker)_DONE`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_DONE")) -Description "mouse flood completion")

    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-UnicodeOutputFlood {
  Write-Host "stress: unicode output $UnicodeLines lines"
  $name = "wmux_stress_unicode_$script:RunId"
  $marker = "WMUX_STRESS_UNICODE_$script:RunId"
  $doneMarker = "ZZWMUX_DONE"
  $attach = $null
  $unicodeScriptPath = Join-Path ([System.IO.Path]::GetTempPath()) "wmux-unicode-$script:RunId.ps1"
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    $attach = Open-Attach -SessionName $name
    [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

    @(
      '$wide=[string][char]0x6f22+[char]0x5b57'
      '$comb=''a''+[char]0x0301'
      '$emoji=[char]::ConvertFromUtf32(0x1F642)'
      "`$marker='$marker'"
      "`$doneMarker='$doneMarker'"
      "1..$UnicodeLines | ForEach-Object { Write-Output (`$marker + '_' + `$_ + ' ' + `$wide + ' ' + `$comb + ' ' + `$emoji) }"
      "Write-Output `$doneMarker"
    ) | Set-Content -LiteralPath $unicodeScriptPath -Encoding UTF8

    $command = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$unicodeScriptPath`"`r"
    Write-AttachInput -Pipe $attach -Text $command
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape($doneMarker)) -Description "unicode output completion")

    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    Remove-Item -LiteralPath $unicodeScriptPath -Force -ErrorAction SilentlyContinue
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-HighOutputAndResizeStorm {
  Write-Host "stress: high output and resize storm"
  $name = "wmux_stress_output_$script:RunId"
  $marker = "WMUX_STRESS_OUTPUT_$script:RunId"
  $doneMarker = "ZZWMUX_DONE"
  $attach = $null
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    $attach = Open-Attach -SessionName $name
    [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

    Write-AttachCommandMode -Pipe $attach -Command "split-window -h"
    [void](Read-UntilMarker -Pipe $attach -Pattern "wmux: split active pane" -Description "split status")
    Write-AttachInput -Pipe $attach -Text "for /L %i in (1,1,$HighOutputLines) do @echo $($marker)_%i`r"

    $sizes = @(
      @{ Columns = 120; Rows = 30 },
      @{ Columns = 90; Rows = 20 },
      @{ Columns = 132; Rows = 43 },
      @{ Columns = 60; Rows = 12 },
      @{ Columns = 160; Rows = 45 },
      @{ Columns = 40; Rows = 8 },
      @{ Columns = 100; Rows = 24 }
    )

    for ($index = 0; $index -lt $ResizeIterations; ++$index) {
      $size = $sizes[$index % $sizes.Count]
      Write-AttachResize -Pipe $attach -Columns $size.Columns -Rows $size.Rows
      if (($index % 8) -eq 0) {
        Drain-AttachOutput -Pipe $attach -Milliseconds 35
      }
    }

    Write-AttachResize -Pipe $attach -Columns 120 -Rows 30
    Write-AttachInput -Pipe $attach -Text "echo $doneMarker`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape($doneMarker)) -Description "high-output completion")

    $status = Invoke-Wmux -Arguments @("server", "status")
    $bytes = [uint64](Get-StatusField -StatusOutput $status.Output -Name "render bytes")
    if ($bytes -eq 0) {
      throw "expected render bytes to increase during high output`n$($status.Output)"
    }

    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-ShellSpawnFailure {
  Write-Host "stress: shell spawn failure $ShellSpawnFailureLoops loops"
  $previous = $env:WMUX_DEFAULT_SHELL
  $badShell = "C:\wmux-missing-shell-$script:RunId.exe"
  try {
    $env:WMUX_DEFAULT_SHELL = $badShell
    [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
    [void](Invoke-Wmux -Arguments @("server", "status"))

    for ($index = 0; $index -lt $ShellSpawnFailureLoops; ++$index) {
      $name = "wmux_stress_spawnfail_$($script:RunId)_$index"
      $result = Invoke-Wmux -Arguments @("new", "-s", $name) -AllowFailure
      if ($result.ExitCode -eq 0) {
        [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
        throw "expected shell spawn failure for $badShell, but session creation succeeded"
      }
      if ($result.Output -notmatch "failed to start shell|shell spawn failed|CreateProcess|cannot find|not found|The system cannot find") {
        throw "unexpected shell spawn failure output`n$($result.Output)"
      }

      Assert-CleanDaemonState
    }
  }
  finally {
    if ($null -eq $previous) {
      Remove-Item Env:WMUX_DEFAULT_SHELL -ErrorAction SilentlyContinue
    } else {
      $env:WMUX_DEFAULT_SHELL = $previous
    }
    [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
    [void](Invoke-Wmux -Arguments @("server", "status"))
  }
  Assert-CleanDaemonState
}

function Test-CopyPasteLoops {
  Write-Host "stress: copy/paste $CopyPasteLoops loops"
  $name = "wmux_stress_copy_$script:RunId"
  $attach = $null
  try {
    [void](Invoke-Wmux -Arguments @("new", "-s", $name))
    $attach = Open-Attach -SessionName $name
    [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")
    Write-AttachInput -Pipe $attach -Text "echo WMUX_COPY_SOURCE_$script:RunId`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern "WMUX_COPY_SOURCE_$script:RunId" -Description "copy source output")

    for ($index = 0; $index -lt $CopyPasteLoops; ++$index) {
      Write-AttachCopyMode -Pipe $attach -Action Enter
      [void](Read-UntilMarker -Pipe $attach -Pattern "copy-mode" -Description "copy mode status")
      Write-AttachCopyMode -Pipe $attach -Action StartSelection
      for ($column = 0; $column -lt 8; ++$column) {
        Write-AttachCopyMode -Pipe $attach -Action CursorRight
      }
      Write-AttachCopyMode -Pipe $attach -Action CopySelection
      [void](Read-UntilMarker -Pipe $attach -Pattern "copied|no copy selection" -Description "copy status")
      Write-AttachPaste -Pipe $attach
      [void](Read-UntilMarker -Pipe $attach -Pattern "pasted|paste buffer empty" -Description "paste status")
      Write-AttachInput -Pipe $attach -Text "$([char]3)"
      Drain-AttachOutput -Pipe $attach -Milliseconds 80
    }

    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
  }
  finally {
    if ($attach -ne $null) {
      $attach.Dispose()
    }
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $name) -AllowFailure)
  }
  Assert-CleanDaemonState
}

function Test-DaemonRestartAndInvalidIpc {
  Write-Host "stress: daemon stop/restart and invalid IPC"
  Assert-CleanDaemonState
  [void](Invoke-Wmux -Arguments @("server", "stop"))
  [void](Invoke-Wmux -Arguments @("server", "status"))
  Invoke-InvalidCommandIpcProbes
  Assert-CleanDaemonState
}

function Test-SectionEnabled {
  param([string]$Name)

  return $Only -contains "All" -or $Only -contains $Name
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$script:RunId = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "stress run: $script:RunId"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
try {
  if (-not $NoRestart) {
    [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  }

  [void](Invoke-Wmux -Arguments @("server", "status"))
  Assert-CleanDaemonState

  if (Test-SectionEnabled -Name "CreateKill") {
    Test-CreateKillSessions
  }
  if (Test-SectionEnabled -Name "AttachDetach") {
    Test-AttachDetachLoops
  }
  if (Test-SectionEnabled -Name "WindowSwitch") {
    Test-WindowSwitchingLoops
  }
  if (Test-SectionEnabled -Name "PaneSplitKill") {
    Test-PaneSplitKillLoops
  }
  if (Test-SectionEnabled -Name "OutputResize") {
    Test-HighOutputAndResizeStorm
  }
  if (Test-SectionEnabled -Name "CopyPaste") {
    Test-CopyPasteLoops
  }
  if (Test-SectionEnabled -Name "MouseFlood") {
    Test-MouseEventFlood
  }
  if (Test-SectionEnabled -Name "UnicodeOutput") {
    Test-UnicodeOutputFlood
  }
  if (Test-SectionEnabled -Name "ShellSpawnFailure") {
    Test-ShellSpawnFailure
  }
  if (Test-SectionEnabled -Name "DaemonIpc") {
    Test-DaemonRestartAndInvalidIpc
  }

  Assert-CleanDaemonState
  Write-Host "ok: stress suite completed"
}
finally {
  if (-not $KeepDaemonRunning) {
    [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  }

  if ($null -eq $previousDefaultShell) {
    Remove-Item Env:WMUX_DEFAULT_SHELL -ErrorAction SilentlyContinue
  } else {
    $env:WMUX_DEFAULT_SHELL = $previousDefaultShell
  }
}

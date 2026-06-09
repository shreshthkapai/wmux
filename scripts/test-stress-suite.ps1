[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

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

  [switch]$NoRestart,

  [switch]$KeepDaemonRunning,

  [ValidateSet("All", "CreateKill", "AttachDetach", "PaneSplitKill", "OutputResize", "CopyPaste", "DaemonIpc")]
  [string[]]$Only = @("All")
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
    [ValidateSet("Input", "Detach", "CommandMode", "Resize", "CopyMode", "Paste")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Input" { 1 }
    "Detach" { 2 }
    "CommandMode" { 6 }
    "Resize" { 4 }
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
  $buffer = [byte[]]::new(32768)
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

    if ($output.Length -gt 1048576) {
      $output = $output.Substring($output.Length - 524288)
    }
  }

  throw "timed out waiting for $Description`nCaptured output:`n$output"
}

function Drain-AttachOutput {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [int]$Milliseconds = 100
  )

  $buffer = [byte[]]::new(32768)
  $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
  while ([DateTime]::UtcNow -lt $deadline) {
    $readTask = $Pipe.ReadAsync($buffer, 0, $buffer.Length)
    if (-not $readTask.Wait(25)) {
      break
    }
    if ($readTask.Result -le 0) {
      break
    }
  }
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
  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    "wmux",
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)
  $pipe.Connect($TimeoutSeconds * 1000)
  try {
    $bytes = [Text.Encoding]::UTF8.GetBytes("not-json`n")
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()

    $buffer = [byte[]]::new(512)
    $readTask = $pipe.ReadAsync($buffer, 0, $buffer.Length)
    if (-not $readTask.Wait($TimeoutSeconds * 1000)) {
      throw "timed out waiting for malformed IPC response"
    }

    $response = [Text.Encoding]::UTF8.GetString($buffer, 0, $readTask.Result)
    if ($response -notmatch '"ok":false' -or $response -notmatch 'malformed daemon request') {
      throw "unexpected malformed IPC response: $response"
    }
  }
  finally {
    $pipe.Dispose()
  }
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

function Test-HighOutputAndResizeStorm {
  Write-Host "stress: high output and resize storm"
  $name = "wmux_stress_output_$script:RunId"
  $marker = "WMUX_STRESS_OUTPUT_$script:RunId"
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
    Write-AttachInput -Pipe $attach -Text "echo $($marker)_DONE`r"
    [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_DONE")) -Description "high-output completion")

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
  Invoke-InvalidCommandIpcProbe
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
  if (Test-SectionEnabled -Name "PaneSplitKill") {
    Test-PaneSplitKillLoops
  }
  if (Test-SectionEnabled -Name "OutputResize") {
    Test-HighOutputAndResizeStorm
  }
  if (Test-SectionEnabled -Name "CopyPaste") {
    Test-CopyPasteLoops
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

[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 120)]
  [int]$TimeoutSeconds = 20,

  [ValidateRange(1, 500)]
  [int]$Iterations = 80
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

function New-AttachFrame {
  param(
    [ValidateSet("Input", "Detach", "Command", "Resize")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Input" { 1 }
    "Detach" { 2 }
    "Command" { 3 }
    "Resize" { 4 }
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

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 512)]
    [int]$Columns,

    [Parameter(Mandatory = $true)]
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
  $buffer = [byte[]]::new(8192)
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

function Drain-AttachOutput {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [int]$Milliseconds = 100
  )

  $buffer = [byte[]]::new(8192)
  $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
  while ([DateTime]::UtcNow -lt $deadline) {
    $readTask = $Pipe.ReadAsync($buffer, 0, $buffer.Length)
    if (-not $readTask.Wait(20)) {
      break
    }
    if ($readTask.Result -le 0) {
      break
    }
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

function Read-LogTailSince {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [long]$Offset
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    return ""
  }

  $stream = [System.IO.File]::Open(
    $Path,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    [System.IO.FileShare]::ReadWrite)
  try {
    if ($Offset -gt 0 -and $Offset -lt $stream.Length) {
      [void]$stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    }
    $reader = [System.IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
    try {
      return $reader.ReadToEnd()
    }
    finally {
      $reader.Dispose()
    }
  }
  finally {
    $stream.Dispose()
  }
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$sessionName = "wmux_resize_stress_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$marker = "WMUX_RESIZE_STRESS_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL
$attach = $null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
$status = Invoke-Wmux -Arguments @("server", "status")
$sessionCount = Get-SessionCount -StatusOutput $status.Output
if ($sessionCount -ne 0) {
  throw "resize-stress test requires an empty daemon because it restarts wmux with WMUX_DEFAULT_SHELL=cmd.exe /D /Q"
}

[void](Invoke-Wmux -Arguments @("server", "stop"))
[void](Invoke-Wmux -Arguments @("server", "status"))
$status = Invoke-Wmux -Arguments @("server", "status")
$daemonLog = Get-StatusField -StatusOutput $status.Output -Name "daemon log"
$logOffset = 0
if (Test-Path -LiteralPath $daemonLog) {
  $logOffset = (Get-Item -LiteralPath $daemonLog).Length
}

try {
  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))

  $attach = Open-Attach -SessionName $sessionName
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

  Write-Host "create nested panes"
  Write-AttachCommand -Pipe $attach -Command "split-horizontal"
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "second pane prompt")
  Write-AttachCommand -Pipe $attach -Command "split-vertical"
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "third pane prompt")

  Write-Host "start heavy output"
  Write-AttachInput -Pipe $attach -Text "for /L %i in (1,1,300) do @echo $($marker)_%i`r"

  Write-Host "send rapid resize frames"
  $sizes = @(
    @{ Columns = 120; Rows = 30 },
    @{ Columns = 90; Rows = 20 },
    @{ Columns = 132; Rows = 43 },
    @{ Columns = 60; Rows = 12 },
    @{ Columns = 160; Rows = 45 },
    @{ Columns = 40; Rows = 8 },
    @{ Columns = 100; Rows = 24 }
  )

  for ($index = 0; $index -lt $Iterations; $index++) {
    $size = $sizes[$index % $sizes.Count]
    Write-AttachResize -Pipe $attach -Columns $size.Columns -Rows $size.Rows
    if (($index % 8) -eq 0) {
      Drain-AttachOutput -Pipe $attach -Milliseconds 40
    }
  }

  Write-AttachResize -Pipe $attach -Columns 100 -Rows 24
  $statusCursor = [regex]::Escape("$([char]27)[24;1H")
  [void](Read-UntilMarker -Pipe $attach -Pattern $statusCursor -Description "final resize redraw")
  Write-AttachInput -Pipe $attach -Text "echo $($marker)_DONE`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_DONE")) -Description "post-resize shell response")

  $logTail = Read-LogTailSince -Path $daemonLog -Offset $logOffset
  $resizeEvents = ([regex]::Matches($logTail, 'component="pty" event="resize"')).Count
  if ($resizeEvents -lt 9) {
    throw "expected repeated per-pane ConPTY resize events, saw $resizeEvents`n$logTail"
  }

  Write-AttachDetach -Pipe $attach
  $attach.Dispose()
  $attach = $null

  Write-Host "ok: rapid resize during heavy output completed with per-pane ConPTY resize events"
}
finally {
  if ($attach -ne $null) {
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

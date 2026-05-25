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
    [ValidateSet("Input", "Detach", "Command")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Input" { 1 }
    "Detach" { 2 }
    "Command" { 3 }
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
$sessionName = "wmux_phase8b_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$marker = "WMUX_PHASE8B_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL
$attach = $null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
$status = Invoke-Wmux -Arguments @("server", "status")
$sessionCount = Get-SessionCount -StatusOutput $status.Output
if ($sessionCount -ne 0) {
  throw "pane-focus test requires an empty daemon because it restarts wmux with WMUX_DEFAULT_SHELL=cmd.exe /D /Q"
}

[void](Invoke-Wmux -Arguments @("server", "stop"))
[void](Invoke-Wmux -Arguments @("server", "status"))

try {
  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))

  $attach = Open-Attach -SessionName $sessionName
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

  Write-Host "pane 1: write marker"
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P1`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P1")) -Description "pane 1 marker")

  Write-Host "Ctrl+b %: split horizontally"
  Write-AttachCommand -Pipe $attach -Command "split-horizontal"
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "pane 2 prompt")
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P2`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P2")) -Description "pane 2 marker")

  Write-Host "Ctrl+b Left: focus pane 1"
  Write-AttachCommand -Pipe $attach -Command "select-pane-left"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P1")) -Description "pane 1 replay")
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P1B`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P1B")) -Description "pane 1 second marker")

  Write-Host "Ctrl+b Right: focus pane 2"
  Write-AttachCommand -Pipe $attach -Command "select-pane-right"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P2")) -Description "pane 2 replay")
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P2B`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P2B")) -Description "pane 2 second marker")

  Write-Host "Ctrl+b `": split vertically"
  Write-AttachCommand -Pipe $attach -Command "split-vertical"
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "pane 3 prompt")
  Write-AttachInput -Pipe $attach -Text "echo $marker`_P3`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P3")) -Description "pane 3 marker")

  Write-Host "Ctrl+b Up: focus pane 2"
  Write-AttachCommand -Pipe $attach -Command "select-pane-up"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$marker`_P2B")) -Description "pane 2 replay after up")

  Write-AttachDetach -Pipe $attach
  $attach.Dispose()
  $attach = $null

  Write-Host "ok: interactive pane split/focus preserved independent shell state"
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

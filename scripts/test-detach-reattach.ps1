[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 60)]
  [int]$TimeoutSeconds = 10,

  [ValidateRange(100, 10000)]
  [int]$DetachedMilliseconds = 1200
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
    [ValidateSet("Input", "Detach")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = if ($Type -eq "Input") { 1 } else { 2 }
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
    [byte[]]$Payload
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type Input -Payload $Payload)
}

function Write-AttachDetach {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe
  )

  Write-PipeBytes -Pipe $Pipe -Bytes (New-AttachFrame -Type Detach)
}

function Write-AttachTextSlow {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  foreach ($ch in $Text.ToCharArray()) {
    Write-AttachInput -Pipe $Pipe -Payload ([Text.Encoding]::UTF8.GetBytes([string]$ch))
    Start-Sleep -Milliseconds 5
  }
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

function Get-MaxMarkerIndex {
  param(
    [string]$Output,

    [Parameter(Mandatory = $true)]
    [string]$Marker
  )

  $pattern = [regex]::Escape($Marker) + "_(?<index>\d+)"
  $matches = [regex]::Matches($Output, $pattern)
  if ($matches.Count -eq 0) {
    return -1
  }

  ($matches | ForEach-Object { [int]$_.Groups["index"].Value } | Measure-Object -Maximum).Maximum
}

function Read-UntilMarkerIndexGreaterThan {
  param(
    [Parameter(Mandatory = $true)]
    [System.IO.Stream]$Pipe,

    [Parameter(Mandatory = $true)]
    [string]$Marker,

    [Parameter(Mandatory = $true)]
    [int]$MinimumIndex,

    [Parameter(Mandatory = $true)]
    [string]$Description
  )

  $output = ""
  $buffer = [byte[]]::new(4096)
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

  while ([DateTime]::UtcNow -lt $deadline) {
    $currentMax = Get-MaxMarkerIndex -Output $output -Marker $Marker
    if ($currentMax -gt $MinimumIndex) {
      return $output
    }

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
  }

  $currentMax = Get-MaxMarkerIndex -Output $output -Marker $Marker
  throw "timed out waiting for $Description; needed index > $MinimumIndex, saw $currentMax`nCaptured output:`n$output"
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
$sessionName = "wmux_phase6_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$marker = "WMUX_PHASE6_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$markerPattern = [regex]::Escape($marker) + "_\d+"
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL
$attach1 = $null
$attach2 = $null
$attach3 = $null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
$status = Invoke-Wmux -Arguments @("server", "status")
$sessionCount = Get-SessionCount -StatusOutput $status.Output
if ($sessionCount -ne 0) {
  throw "detach/reattach test requires an empty daemon because it restarts wmux with WMUX_DEFAULT_SHELL=cmd.exe /D /Q"
}

[void](Invoke-Wmux -Arguments @("server", "stop"))
[void](Invoke-Wmux -Arguments @("server", "status"))

try {
  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))

  Write-Host "attach #1: start long-running output, then close pipe abruptly"
  $attach1 = Open-Attach -SessionName $sessionName
  [void](Read-UntilMarker -Pipe $attach1 -Pattern ">" -Description "cmd prompt")
  $command = "echo $marker`_0`rfor /l %i in (1,1,1000000) do @(echo $marker`_%i & ping -n 2 127.0.0.1 ^>nul)`r"
  Write-AttachTextSlow -Pipe $attach1 -Text $command
  $firstOutput = Read-UntilMarker -Pipe $attach1 -Pattern $markerPattern -Description "initial marker"
  $firstMax = Get-MaxMarkerIndex -Output $firstOutput -Marker $marker
  $attach1.Dispose()

  Start-Sleep -Milliseconds $DetachedMilliseconds

  Write-Host "attach #2: verify output continued while detached"
  $attach2 = Open-Attach -SessionName $sessionName
  $secondOutput = Read-UntilMarkerIndexGreaterThan `
    -Pipe $attach2 `
    -Marker $marker `
    -MinimumIndex $firstMax `
    -Description "reattach marker newer than first attach"
  $secondMax = Get-MaxMarkerIndex -Output $secondOutput -Marker $marker
  Write-AttachDetach -Pipe $attach2
  $attach2.Dispose()

  Start-Sleep -Milliseconds $DetachedMilliseconds

  Write-Host "attach #3: verify explicit detach left session attachable"
  $attach3 = Open-Attach -SessionName $sessionName
  $thirdOutput = Read-UntilMarkerIndexGreaterThan `
    -Pipe $attach3 `
    -Marker $marker `
    -MinimumIndex $secondMax `
    -Description "post-detach marker newer than second attach"
  $thirdMax = Get-MaxMarkerIndex -Output $thirdOutput -Marker $marker

  Write-AttachInput -Pipe $attach3 -Payload ([byte[]](0x03))
  Write-AttachDetach -Pipe $attach3
  $attach3.Dispose()

  Write-Host "ok: detach/reattach preserved daemon-owned shell output ($firstMax -> $secondMax -> $thirdMax)"
}
finally {
  if ($attach1 -ne $null) {
    $attach1.Dispose()
  }
  if ($attach2 -ne $null) {
    $attach2.Dispose()
  }
  if ($attach3 -ne $null) {
    $attach3.Dispose()
  }

  [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName) -AllowFailure)
  [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  if ($null -eq $previousDefaultShell) {
    Remove-Item Env:WMUX_DEFAULT_SHELL -ErrorAction SilentlyContinue
  } else {
    $env:WMUX_DEFAULT_SHELL = $previousDefaultShell
  }
}

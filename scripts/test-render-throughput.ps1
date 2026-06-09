[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 120)]
  [int]$TimeoutSeconds = 20,

  [ValidateRange(10, 5000)]
  [int]$Lines = 800
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
    [ValidateSet("Input", "Detach")]
    [string]$Type,

    [byte[]]$Payload = @()
  )

  $typeByte = switch ($Type) {
    "Input" { 1 }
    "Detach" { 2 }
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

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$sessionName = "wmux_render_perf_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$marker = "WMUX_RENDER_PERF_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$previousDefaultShell = $env:WMUX_DEFAULT_SHELL
$attach = $null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

$env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
$status = Invoke-Wmux -Arguments @("server", "status")
$sessionCount = [int](Get-StatusField -StatusOutput $status.Output -Name "sessions")
if ($sessionCount -ne 0) {
  throw "render-throughput test requires an empty daemon because it restarts wmux with WMUX_DEFAULT_SHELL=cmd.exe /D /Q"
}

[void](Invoke-Wmux -Arguments @("server", "stop"))
[void](Invoke-Wmux -Arguments @("server", "status"))

try {
  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))
  $attach = Open-Attach -SessionName $sessionName
  [void](Read-UntilMarker -Pipe $attach -Pattern ">" -Description "initial cmd prompt")

  $started = [Diagnostics.Stopwatch]::StartNew()
  Write-AttachInput -Pipe $attach -Text "for /L %i in (1,1,$Lines) do @echo $($marker)_%i`r"
  Write-AttachInput -Pipe $attach -Text "echo $($marker)_DONE`r"
  [void](Read-UntilMarker -Pipe $attach -Pattern ([regex]::Escape("$($marker)_DONE")) -Description "high-output completion")
  $started.Stop()

  $status = Invoke-Wmux -Arguments @("server", "status")
  $partialFrames = [uint64](Get-StatusField -StatusOutput $status.Output -Name "render partial frames")
  $fullFrames = [uint64](Get-StatusField -StatusOutput $status.Output -Name "render full frames")
  $renderBytes = [uint64](Get-StatusField -StatusOutput $status.Output -Name "render bytes")

  if ($partialFrames -eq 0) {
    throw "expected high-output attach to use partial render frames`n$status"
  }
  if ($renderBytes -eq 0) {
    throw "expected render byte counter to increase`n$status"
  }

  Write-AttachDetach -Pipe $attach
  $attach.Dispose()
  $attach = $null

  Write-Host "ok: rendered $Lines lines in $($started.ElapsedMilliseconds) ms; full=$fullFrames partial=$partialFrames bytes=$renderBytes"
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

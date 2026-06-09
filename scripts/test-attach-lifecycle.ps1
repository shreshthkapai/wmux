[CmdletBinding()]
param(
  [string]$Wmux = (Join-Path $PSScriptRoot "..\build-vs\Debug\wmux.exe"),

  [ValidateRange(1, 100)]
  [int]$Iterations = 5,

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

  $frame = [byte[]]::new(7)
  $frame[0] = [byte][char]"W"
  $frame[1] = [byte][char]"M"
  $frame[2] = 2
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

  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    "wmux-attach",
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)

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

function Wait-ForStatusField {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [string]$Value
  )

  $pattern = "(?m)^$([regex]::Escape($Name)):\s+$([regex]::Escape($Value))\s*$"
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $status = (Invoke-Wmux -Arguments @("server", "status")).Output
    if ($status -match $pattern) {
      return
    }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)

  throw "expected '${Name}: $Value' in server status`n$status"
}

$script:WmuxPath = (Resolve-Path -LiteralPath $Wmux).Path
$sessionName = "wmux_attach_lifecycle_$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$attach = $null

Write-Host "wmux executable: $script:WmuxPath"
Write-Host "session: $sessionName"

try {
  [void](Invoke-Wmux -Arguments @("server", "stop", "--force") -AllowFailure)
  $env:WMUX_DEFAULT_SHELL = "cmd.exe /D /Q"
  [void](Invoke-Wmux -Arguments @("new", "-s", $sessionName))

  for ($index = 1; $index -le $Iterations; $index++) {
    Write-Host ("[{0}/{1}] explicit detach cleanup" -f $index, $Iterations)
    $attach = Open-Attach -SessionName $sessionName
    Write-AttachDetach -Pipe $attach
    $attach.Dispose()
    $attach = $null
    Wait-ForStatusField -Name "attach clients" -Value "0"
    Wait-ForStatusField -Name "attach workers" -Value "0"

    Write-Host ("[{0}/{1}] abrupt pipe close cleanup" -f $index, $Iterations)
    $attach = Open-Attach -SessionName $sessionName
    $attach.Dispose()
    $attach = $null
    Wait-ForStatusField -Name "attach clients" -Value "0"
    Wait-ForStatusField -Name "attach workers" -Value "0"
  }

  [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName))
  Wait-ForStatusField -Name "sessions" -Value "0"
  Wait-ForStatusField -Name "live shells" -Value "0"
  Wait-ForStatusField -Name "attach workers" -Value "0"

  Write-Host "ok: attach clients and workers cleaned up after detach and abrupt disconnect"
}
finally {
  if ($attach -ne $null) {
    $attach.Dispose()
  }
  try {
    [void](Invoke-Wmux -Arguments @("kill-session", "-t", $sessionName) -AllowFailure)
  } catch {
  }
}

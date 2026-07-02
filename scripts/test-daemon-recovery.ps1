param(
  [string]$Wmux = ".\build-vs\Debug\wmux.exe"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\wmux-script-helpers.ps1"

function Invoke-Wmux {
  param(
    [string[]]$CommandArgs,
    [switch]$AllowFailure
  )

  $previousErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $output = & $Wmux @CommandArgs 2>&1
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }
  if (-not $AllowFailure -and $code -ne 0) {
    throw "wmux $($CommandArgs -join ' ') failed with ${code}: $output"
  }

  [pscustomobject]@{
    Code = $code
    Text = ($output -join "`n")
  }
}

function Assert-Contains {
  param(
    [string]$Text,
    [string]$Needle
  )

  if ($Text -notlike "*$Needle*") {
    throw "expected output to contain '$Needle' but got: $Text"
  }
}

function Assert-StatusValue {
  param(
    [string]$Status,
    [string]$Name,
    [string]$Value
  )

  if ($Status -notmatch "(?m)^$([regex]::Escape($Name)):\s+$([regex]::Escape($Value))$") {
    throw "expected '${Name}: $Value' in status but got: $Status"
  }
}

function Wait-StatusValue {
  param(
    [string]$Name,
    [string]$Value,
    [int]$Attempts = 50
  )

  for ($i = 0; $i -lt $Attempts; ++$i) {
    $status = (Invoke-Wmux -CommandArgs @("server", "status")).Text
    if ($status -match "(?m)^$([regex]::Escape($Name)):\s+$([regex]::Escape($Value))$") {
      return $status
    }
    Start-Sleep -Milliseconds 100
  }

  throw "timed out waiting for '${Name}: $Value'"
}

function Write-Config {
  param(
    [string]$Profile,
    [string]$Shell
  )

  @"
set -g default-shell "$Shell"
set -g mouse off
set -g status on
"@ | Set-Content -LiteralPath (Join-Path $Profile ".wmux.conf") -Encoding ASCII
}

function Open-And-Close-AttachPipe {
  $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    ".",
    (Get-WmuxAttachPipeName),
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::Asynchronous
  )
  $pipe.Connect(2000)
  $request = '{"type":"AttachStart","session_name":"missing","terminal_columns":120,"terminal_rows":30}' + "`n"
  $bytes = [Text.Encoding]::UTF8.GetBytes($request)
  $pipe.Write($bytes, 0, $bytes.Length)
  $pipe.Dispose()
}

$originalUserProfile = $env:USERPROFILE
$profile = Join-Path $env:TEMP ("wmux-recovery-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $profile | Out-Null
$env:USERPROFILE = $profile

try {
  Write-Config -Profile $profile -Shell "wmux-definitely-missing-shell.exe"
  Invoke-Wmux -CommandArgs @("server", "stop", "--force") -AllowFailure | Out-Null

  $spawn = Invoke-Wmux -CommandArgs @("new", "-s", "badspawn") -AllowFailure
  if ($spawn.Code -eq 0) {
    throw "expected shell spawn failure to reject session creation"
  }

  $status = (Invoke-Wmux -CommandArgs @("server", "status")).Text
  Assert-StatusValue -Status $status -Name "sessions" -Value "0"
  Assert-StatusValue -Status $status -Name "live shells" -Value "0"
  Invoke-Wmux -CommandArgs @("server", "stop") | Out-Null

  $status = (Invoke-Wmux -CommandArgs @("server", "status")).Text
  Assert-StatusValue -Status $status -Name "sessions" -Value "0"

  Invoke-Wmux -CommandArgs @("server", "stop", "--force") | Out-Null
  Write-Config -Profile $profile -Shell "cmd.exe /D /Q"
  Invoke-Wmux -CommandArgs @("new", "-s", "recover") | Out-Null

  Open-And-Close-AttachPipe
  $status = Wait-StatusValue -Name "attach clients" -Value "0"
  Assert-StatusValue -Status $status -Name "attach workers" -Value "0"

  $safeStop = Invoke-Wmux -CommandArgs @("server", "stop") -AllowFailure
  if ($safeStop.Code -eq 0) {
    throw "expected safe server stop to refuse live sessions"
  }
  Assert-Contains -Text $safeStop.Text -Needle "live sessions exist"

  $status = (Invoke-Wmux -CommandArgs @("server", "status")).Text
  Assert-StatusValue -Status $status -Name "sessions" -Value "1"
  Assert-StatusValue -Status $status -Name "live shells" -Value "1"

  Invoke-Wmux -CommandArgs @("server", "stop", "--force") | Out-Null
  $status = (Invoke-Wmux -CommandArgs @("server", "status")).Text
  Assert-StatusValue -Status $status -Name "sessions" -Value "0"
  Assert-StatusValue -Status $status -Name "live shells" -Value "0"
  Assert-StatusValue -Status $status -Name "attach workers" -Value "0"

  Write-Host "ok: daemon recovery, spawn failure, failed attach, and safe stop paths passed"
} finally {
  try {
    Invoke-Wmux -CommandArgs @("server", "stop", "--force") -AllowFailure | Out-Null
  } catch {
  }
  $env:USERPROFILE = $originalUserProfile
  Remove-Item -LiteralPath $profile -Recurse -Force -ErrorAction SilentlyContinue
}

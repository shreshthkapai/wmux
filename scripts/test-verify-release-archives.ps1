[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$targets = [ordered]@{
    'x86_64-pc-windows-msvc'      = @('wmux.exe', 'wmux-server.exe')
    'x86_64-unknown-linux-musl'   = @('wmux', 'wmux-server')
    'aarch64-unknown-linux-musl'  = @('wmux', 'wmux-server')
    'x86_64-apple-darwin'         = @('wmux', 'wmux-server')
    'aarch64-apple-darwin'        = @('wmux', 'wmux-server')
}
$documents = @('README.md', 'LICENSE', 'CHANGELOG.md')
$verifier = Join-Path $PSScriptRoot 'verify-release-archives.ps1'
$temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $temporaryRoot ("wmux-release-fixtures-{0}" -f [System.Guid]::NewGuid())
$goodRoot = Join-Path $fixtureRoot 'good'
$badRoot = Join-Path $fixtureRoot 'bad'

function New-ReleaseFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [switch]$OmitWindowsServer
    )

    New-Item -ItemType Directory -Path $Root | Out-Null

    foreach ($target in $targets.GetEnumerator()) {
        $staging = Join-Path $Root ("staging-{0}" -f $target.Key)
        New-Item -ItemType Directory -Path $staging | Out-Null

        foreach ($document in $documents) {
            Set-Content -LiteralPath (Join-Path $staging $document) -Value $document -NoNewline
        }
        foreach ($binary in $target.Value) {
            if ($OmitWindowsServer -and
                $target.Key -eq 'x86_64-pc-windows-msvc' -and
                $binary -eq 'wmux-server.exe') {
                continue
            }
            Set-Content -LiteralPath (Join-Path $staging $binary) -Value $binary -NoNewline
        }

        if ($target.Key -eq 'x86_64-pc-windows-msvc') {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $archive = Join-Path $Root ("wmux-{0}.zip" -f $target.Key)
            [System.IO.Compression.ZipFile]::CreateFromDirectory($staging, $archive)
        }
        else {
            $archive = Join-Path $Root ("wmux-{0}.tar.gz" -f $target.Key)
            & tar -czf $archive -C $staging .
            if ($LASTEXITCODE -ne 0) {
                throw "could not create fixture archive $archive"
            }
        }

        $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        Set-Content -LiteralPath "$archive.sha256" -Value "$hash  $([System.IO.Path]::GetFileName($archive))"
    }
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    New-ReleaseFixture -Root $goodRoot
    New-ReleaseFixture -Root $badRoot -OmitWindowsServer

    $goodOutput = @(& $verifier -ArtifactsDirectory $goodRoot)
    $targetLines = @($goodOutput | Where-Object { $_ -like 'verified *:*' })
    if ($targetLines.Count -ne 5) {
        throw "good fixture produced $($targetLines.Count) target lines instead of 5"
    }

    $badMessage = $null
    try {
        & $verifier -ArtifactsDirectory $badRoot | Out-Null
    }
    catch {
        $badMessage = $_.Exception.Message
    }
    if ($null -eq $badMessage) {
        throw 'bad fixture unexpectedly passed archive verification'
    }
    if ($badMessage -notmatch 'missing wmux-server\.exe') {
        throw "bad fixture failed for the wrong reason: $badMessage"
    }

    Write-Output 'archive verifier self-test passed'
}
finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        $resolvedFixture = [System.IO.Path]::GetFullPath($fixtureRoot)
        if (-not $resolvedFixture.StartsWith($temporaryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to remove fixture outside the temporary directory: $resolvedFixture"
        }
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}

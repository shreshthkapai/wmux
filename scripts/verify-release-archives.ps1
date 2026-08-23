[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArtifactsDirectory
)

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

function Get-ArchiveLeafNames {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileInfo]$Archive
    )

    if ($Archive.Name.EndsWith('.zip', [System.StringComparison]::OrdinalIgnoreCase)) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead($Archive.FullName)
        try {
            return @(
                $zip.Entries |
                    ForEach-Object { [System.IO.Path]::GetFileName($_.FullName.TrimEnd('/', '\')) } |
                    Where-Object { $_ }
            )
        }
        finally {
            $zip.Dispose()
        }
    }

    $entries = @(& tar -tf $Archive.FullName)
    if ($LASTEXITCODE -ne 0) {
        throw "could not list archive $($Archive.Name)"
    }

    return @(
        $entries |
            ForEach-Object {
                [System.IO.Path]::GetFileName(($_ -replace '\\', '/').TrimEnd('/'))
            } |
            Where-Object { $_ }
    )
}

function Assert-ArchiveEntry {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Entries,

        [Parameter(Mandatory = $true)]
        [string]$Required,

        [Parameter(Mandatory = $true)]
        [string]$ArchiveName
    )

    if ($Entries -cnotcontains $Required) {
        throw "$ArchiveName is missing $Required"
    }
}

$resolvedDirectory = Resolve-Path -LiteralPath $ArtifactsDirectory -ErrorAction Stop
if (-not (Test-Path -LiteralPath $resolvedDirectory.Path -PathType Container)) {
    throw "artifact path is not a directory: $ArtifactsDirectory"
}

$files = @(Get-ChildItem -LiteralPath $resolvedDirectory.Path -File)

foreach ($target in $targets.GetEnumerator()) {
    $archives = @(
        $files | Where-Object {
            $_.Name.Contains($target.Key) -and
            ($_.Name.EndsWith('.zip', [System.StringComparison]::OrdinalIgnoreCase) -or
                $_.Name.EndsWith('.tar.gz', [System.StringComparison]::OrdinalIgnoreCase))
        }
    )

    if ($archives.Count -ne 1) {
        throw "expected exactly one archive for $($target.Key), found $($archives.Count)"
    }

    $archive = $archives[0]
    $checksumPath = "$($archive.FullName).sha256"
    if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
        throw "$($archive.Name) is missing checksum sidecar $($archive.Name).sha256"
    }

    $checksumLine = Get-Content -LiteralPath $checksumPath -TotalCount 1
    $expectedHash = (($checksumLine -split '\s+')[0]).ToLowerInvariant()
    if ($expectedHash -notmatch '^[0-9a-f]{64}$') {
        throw "$($archive.Name).sha256 does not start with a valid SHA-256 hash"
    }

    $actualHash = (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $expectedHash) {
        throw "$($archive.Name) has an invalid SHA-256 checksum"
    }

    $entries = @(Get-ArchiveLeafNames -Archive $archive)
    foreach ($binary in $target.Value) {
        Assert-ArchiveEntry -Entries $entries -Required $binary -ArchiveName $archive.Name
    }
    foreach ($document in $documents) {
        Assert-ArchiveEntry -Entries $entries -Required $document -ArchiveName $archive.Name
    }

    Write-Output "verified $($target.Key): $($archive.Name)"
}

Write-Output "verified all $($targets.Count) wmux release archives"

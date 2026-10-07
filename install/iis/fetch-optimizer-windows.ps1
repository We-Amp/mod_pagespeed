# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Fetch the optimizer worker for the IIS package from the pinned optimizer
# release, verified the same way the Linux packages are.
#
# Usage:
#   .\install\iis\fetch-optimizer-windows.ps1 -OutDir <dir> [-Pin <optimizer-pin.json>]
#
# Reads the pin (default install\debian\optimizer-pin.json), downloads that
# release's SHA256SUMS and checks it against the pinned hash, then downloads
# the release's win-x64 zip, checks it against SHA256SUMS and extracts it
# into -OutDir (factory_worker.exe, pagespeed.dll, LICENSE, NOTICE). Pass
# -OutDir to build-msi.ps1 as -OptimizerDir.
#
# Exit status: 0 = fetched and verified; 1 = any verification or download
# failure; 3 = the pinned release carries no Windows zip (a release cut
# before the optimizer published one), so the package can only be the
# module alone.

param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Pin = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Pin) { $Pin = Join-Path $ScriptDir "..\debian\optimizer-pin.json" }

$pinDoc = Get-Content $Pin -Raw | ConvertFrom-Json
$tag = $pinDoc.tag
$sumsHash = $pinDoc.sha256sums_sha256
if (-not $tag -or $tag -like "PLACEHOLDER*" -or -not $sumsHash) {
    Write-Host "::error::optimizer-pin.json names no release (tag '$tag')"
    exit 1
}
$base = "https://github.com/We-Amp/pagespeed-optimizer/releases/download/$tag"

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("optimizer-fetch-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $sums = Join-Path $work "SHA256SUMS"
    Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
    $got = (Get-FileHash $sums -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $sumsHash.ToLower()) {
        Write-Host "::error::SHA256SUMS of $tag is $got, the pin says $sumsHash"
        exit 1
    }

    # "<hash>  <name>" lines; the Windows zip is the one win-x64 entry.
    $entries = @(Get-Content $sums | Where-Object { $_ -cmatch '^([0-9a-f]{64})\s+\*?([A-Za-z0-9._+-]+-win-x64\.zip)$' } | ForEach-Object {
        [pscustomobject]@{ Hash = $Matches[1]; Name = $Matches[2] }
    })
    if ($entries.Count -eq 0) {
        Write-Host "The pinned optimizer release $tag carries no Windows zip."
        exit 3
    }
    if ($entries.Count -gt 1) {
        Write-Host "::error::$tag lists more than one win-x64 zip: $($entries.Name -join ', ')"
        exit 1
    }
    $zip = Join-Path $work $entries[0].Name
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$($entries[0].Name)" -OutFile $zip
    $zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($zipHash -ne $entries[0].Hash) {
        Write-Host "::error::$($entries[0].Name) is $zipHash, SHA256SUMS says $($entries[0].Hash)"
        exit 1
    }

    # Only the four expected entries, each written to a fixed name: no
    # path from inside the archive reaches the file system.
    if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($f in @("factory_worker.exe", "pagespeed.dll", "LICENSE", "NOTICE")) {
            $entry = $archive.Entries | Where-Object { $_.FullName -ceq $f } | Select-Object -First 1
            if (-not $entry) {
                Write-Host "::error::$($entries[0].Name) has no $f"
                exit 1
            }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $OutDir $f), $true)
        }
    } finally {
        $archive.Dispose()
    }
    Write-Host "Fetched and verified $($entries[0].Name) from $tag into $OutDir"
    exit 0
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

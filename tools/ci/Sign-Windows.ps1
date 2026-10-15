# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Authenticode-sign files with Microsoft Trusted Signing and verify each
    signature.

.DESCRIPTION
    Runs only in the hosted signing jobs of the release lane
    (docs/ci/release.md), which hold the six environment variables below as
    secrets of the protected `release` environment. Signs every file given with
    signtool and the Trusted Signing client library, timestamps it, then asks
    Windows for the signature status of each file and fails unless it is Valid.
    Prints no secret and no endpoint.

    Environment (all required): AZURE_TENANT_ID, AZURE_CLIENT_ID,
    AZURE_CLIENT_SECRET, TRUSTED_SIGNING_ENDPOINT, TRUSTED_SIGNING_ACCOUNT,
    TRUSTED_SIGNING_PROFILE.

.PARAMETER Files
    The files to sign, in place.

.EXAMPLE
    .\tools\ci\Sign-Windows.ps1 -Files signed-output\iis\pagespeed_iis.dll
#>
param(
    [Parameter(Mandatory = $true)][string[]]$Files
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

foreach ($n in @('AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'AZURE_CLIENT_SECRET',
                 'TRUSTED_SIGNING_ENDPOINT', 'TRUSTED_SIGNING_ACCOUNT', 'TRUSTED_SIGNING_PROFILE')) {
    if (-not [Environment]::GetEnvironmentVariable($n)) {
        Write-Host "::error::the secret behind $n is not set in the release environment (docs/ci/release.md)"
        exit 1
    }
}
foreach ($f in $Files) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        Write-Host "::error::nothing to sign at $f"
        exit 1
    }
}

# The Trusted Signing client library, a pinned version of the signed
# Microsoft package.
$dlibDir = Join-Path $env:RUNNER_TEMP 'TrustedSigning'
New-Item -ItemType Directory -Path $dlibDir -Force | Out-Null
$pkg = Join-Path $dlibDir 'Microsoft.Trusted.Signing.Client.zip'
Invoke-WebRequest -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Trusted.Signing.Client/1.0.53' -OutFile $pkg
Expand-Archive -Path $pkg -DestinationPath $dlibDir -Force
$dlib = Join-Path $dlibDir 'bin\x64\Azure.CodeSigning.Dlib.dll'
if (-not (Test-Path -LiteralPath $dlib)) { Write-Host "::error::the signing client library is missing"; exit 1 }

$metadataFile = Join-Path $env:RUNNER_TEMP 'tsmetadata.json'
@{
    Endpoint               = $env:TRUSTED_SIGNING_ENDPOINT
    CodeSigningAccountName = $env:TRUSTED_SIGNING_ACCOUNT
    CertificateProfileName = $env:TRUSTED_SIGNING_PROFILE
    CorrelationId          = [guid]::NewGuid().ToString()
} | ConvertTo-Json | Set-Content -Path $metadataFile

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { Write-Host "::error::signtool.exe not found"; exit 1 }

try {
    foreach ($f in $Files) {
        Set-ItemProperty -LiteralPath $f -Name IsReadOnly -Value $false
        Write-Host "Signing $f"
        & $signtool.FullName sign /v /fd SHA256 `
            /tr 'http://timestamp.acs.microsoft.com' /td SHA256 `
            /dlib $dlib /dmdf $metadataFile `
            $f
        if ($LASTEXITCODE -ne 0) { Write-Host "::error::signtool failed on $f (exit $LASTEXITCODE)"; exit 1 }
    }
} finally {
    Remove-Item -LiteralPath $metadataFile -Force -ErrorAction SilentlyContinue
}

# Consume what was signed: Windows must call every signature valid.
foreach ($f in $Files) {
    $sig = Get-AuthenticodeSignature -LiteralPath $f
    if ($sig.Status -ne 'Valid') {
        Write-Host "::error::$f signature status: $($sig.Status)"
        exit 1
    }
    Write-Host "$f signed by: $($sig.SignerCertificate.Subject)"
}

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
    Windows for the signature of each file and fails unless its status is Valid
    and its signer certificate's subject is exactly $ExpectedSubject.
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

# The signer every release file must carry: the We-Amp B.V. identity of the
# Trusted Signing certificate profile (as Windows renders the subject). A
# signature that Windows calls Valid but that names anyone else, for example
# a misconfigured profile or another tenant's, fails the job.
$ExpectedSubject = 'CN=We-Amp B.V., O=We-Amp B.V., L=Castricum, C=NL'

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
# The package runs inside the signing step, next to the secrets: it must be
# exactly the one pinned here.
$pinned = 'b5abe405d1464e59c2c4f4fe8b4653bf184fe3e975a75c9628d366ed384c6979'
$actual = (Get-FileHash -LiteralPath $pkg -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $pinned) { Write-Host "::error::the signing client package does not match the pinned hash"; exit 1 }
Expand-Archive -Path $pkg -DestinationPath $dlibDir -Force
$dlib = Join-Path $dlibDir 'bin\x64\Azure.CodeSigning.Dlib.dll'
if (-not (Test-Path -LiteralPath $dlib)) { Write-Host "::error::the signing client library is missing"; exit 1 }
# Second guard: the library must carry a valid Microsoft signature.
$dsig = Get-AuthenticodeSignature -LiteralPath $dlib
if ($dsig.Status -ne 'Valid' -or $dsig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    Write-Host "::error::the signing client library is not validly signed by Microsoft"
    exit 1
}

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

# Consume what was signed: Windows must call every signature valid, and the
# signer must be exactly the expected identity.
foreach ($f in $Files) {
    $sig = Get-AuthenticodeSignature -LiteralPath $f
    if ($sig.Status -ne 'Valid') {
        Write-Host "::error::$f signature status: $($sig.Status)"
        exit 1
    }
    $subject = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '<no signer certificate>' }
    if ($subject -cne $ExpectedSubject) {
        Write-Host "::error::$f is signed by '$subject', expected '$ExpectedSubject'"
        exit 1
    }
    Write-Host "$f signed by: $subject"
}

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build MSI installer for IIS PageSpeed module.
#
# Usage:
#   .\install\iis\build-msi.ps1 [-OutputDir C:\output] [-BinDir bazel-bin\pagespeed\iis]
#       [-OptimizerDir <dir with factory_worker.exe + pagespeed.dll>]
#       [-Revision <0..65535>]
#
# -Revision is the build revision, the fourth field of the MSI ProductVersion
# (MAJOR.MINOR.BUILD.REVISION). It must grow from one build to the next (a
# release run number, never a date or a hash). Default: the PAGESPEED_MSI_REVISION
# environment variable, else 0. The version the product shows (the package
# name, the file name, the X-Page-Speed header) stays MAJOR.MINOR.BUILD; the
# revision appears only as the version in Programs and Features. Windows
# Installer ignores the fourth field when it compares versions, so Product.wxs
# lets a build of the same MAJOR.MINOR.BUILD upgrade the installed one and
# refuses one with a lower revision itself.
#
# With -OptimizerDir the package also installs the optimizer worker as a
# disabled Windows service and the client library the module loads for the
# daemon path; the directory is what fetch-optimizer-windows.ps1 extracts
# from the pinned optimizer release. Without it the package is the module
# alone.
#
# Prerequisites:
#   - .NET SDK 8+ (for dotnet tool)
#   - WiX Toolset v5.0.2 (installed automatically if missing)
#   - Bazel build of //pagespeed/iis:pagespeed_iis.dll completed

param(
    [string]$OutputDir = (Get-Location).Path,
    [string]$BinDir = "",
    [string]$SrcDir = "",
    [string]$OptimizerDir = "",
    [string]$Revision = $env:PAGESPEED_MSI_REVISION
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $SrcDir) { $SrcDir = (Resolve-Path "$ScriptDir\..\..").Path }
if (-not $BinDir) { $BinDir = Join-Path $SrcDir "bazel-bin\pagespeed\iis" }

# --- Read VERSION ---
$versionData = @{}
Get-Content (Join-Path $SrcDir "net\instaweb\public\VERSION") | ForEach-Object {
    if ($_ -match '^\s*(\w+)\s*=\s*(.+)\s*$') {
        $versionData[$Matches[1]] = $Matches[2].Trim()
    }
}
$MAJOR = $versionData['MAJOR']
$MINOR = $versionData['MINOR']
$BUILD = $versionData['BUILD']
$PRERELEASE = $versionData['PRERELEASE']
if (-not $MAJOR -or -not $MINOR) {
    Write-Error "Failed to parse MAJOR/MINOR from VERSION file"
    exit 1
}

if (-not $Revision) { $Revision = "0" }
if ($Revision -notmatch '^\d{1,5}$' -or [int]$Revision -gt 65535) {
    Write-Error "Revision must be an integer from 0 to 65535 (the fourth field of an MSI ProductVersion), got '$Revision'"
    exit 1
}
$Revision = [string][int]$Revision

$ProductVersion = "$MAJOR.$MINOR.$BUILD.$Revision"
if ($PRERELEASE) {
    $DisplayVersion = "$MAJOR.$MINOR.$BUILD-$PRERELEASE"
} else {
    $DisplayVersion = "$MAJOR.$MINOR.$BUILD"
}

# --- Read UpgradeCode (permanent, never changes) ---
$UpgradeCode = (Get-Content (Join-Path $ScriptDir "UpgradeCode.txt") -Raw).Trim()

# --- Verify DLL exists ---
$DllPath = Join-Path $BinDir "pagespeed_iis.dll"
if (-not (Test-Path $DllPath)) {
    Write-Error "DLL not found at $DllPath. Build it first:`n  bazel build --config=windows --config=clang-cl -c opt //pagespeed/iis:pagespeed_iis.dll"
    exit 1
}

# --- Verify the optimizer files, when given ---
$OptimizerDefine = @()
if ($OptimizerDir) {
    foreach ($f in @("factory_worker.exe", "pagespeed.dll", "LICENSE", "NOTICE")) {
        if (-not (Test-Path (Join-Path $OptimizerDir $f))) {
            Write-Error "$f not found in $OptimizerDir (expected the pinned optimizer release's win-x64 zip, extracted)"
            exit 1
        }
    }
    $OptimizerDir = (Resolve-Path $OptimizerDir).Path
    $OptimizerDefine = @("-d", "OptimizerDir=$OptimizerDir")
}

# --- Ensure WiX toolset v5 is installed (pinned for extension compatibility) ---
$wixVersion = "5.0.2"
Write-Host "Ensuring WiX Toolset v${wixVersion}..."
$currentWix = Get-Command wix -ErrorAction SilentlyContinue
if ($currentWix) {
    # Strip build metadata (e.g. "5.0.2+hash123" → "5.0.2")
    $rawVersion = (wix --version 2>$null)
    if ($rawVersion -match '(\d+\.\d+\.\d+)') { $currentVersion = $Matches[1] } else { $currentVersion = "" }
    if ($currentVersion -and $currentVersion -ne $wixVersion) {
        Write-Host "Removing WiX v${currentVersion} (need v${wixVersion})..."
        dotnet tool uninstall --global wix 2>$null
        $currentWix = $null
    }
}
if (-not $currentWix) {
    dotnet tool install --global wix --version $wixVersion
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to install WiX Toolset. Ensure .NET SDK 8+ is installed."
        exit 1
    }
}

# No WiX extensions currently required. WixToolset.Util.wixext was
# considered (for util:PermissionEx declarative ACL rows) and rejected:
# upstream WiX v5.0.2 emits util:PermissionEx as Wix4SchedSecureObjects /
# Wix4ExecSecureObjects references that pull Wix4UtilCA_X64.dll
# (CRT-dependent) into the MSI Binary table, which breaks stock Windows
# Server 2016 installs (the exact failure mode the Type-34 deferred-exe
# CAs in Product.wxs were designed to avoid). The deferred icacls CAs
# (GrantCacheAcl / GrantLogAcl) are the sole ACL mechanism.

# --- Build MSI ---
# ICE validation is not run (wix build does not validate). If it is ever turned
# on (wix msi validate), suppress ICE61 explicitly (-sice ICE61): it warns
# about the Upgrade row MajorUpgrade AllowSameVersionUpgrades writes, which
# is deliberate here (a build of the same MAJOR.MINOR.BUILD replaces the
# installed one; Product.wxs refuses a lower revision itself).
$WxsPath = Join-Path $ScriptDir "Product.wxs"
$MsiName = "pagespeed-iis-$DisplayVersion-win-x64.msi"
$MsiPath = Join-Path $OutputDir $MsiName

Write-Host "Building MSI: $MsiName"
Write-Host "  ProductVersion: $ProductVersion"
Write-Host "  DisplayVersion: $DisplayVersion"
Write-Host "  UpgradeCode:    $UpgradeCode"
Write-Host "  BinDir:         $BinDir"
Write-Host "  SrcDir:         $SrcDir"
Write-Host "  OptimizerDir:   $(if ($OptimizerDir) { $OptimizerDir } else { '(none: module only)' })"

wix build $WxsPath `
    -d "ProductVersion=$ProductVersion" `
    -d "ShortVersion=$MAJOR.$MINOR.$BUILD" `
    -d "Revision=$Revision" `
    -d "DisplayVersion=$DisplayVersion" `
    -d "UpgradeCode=$UpgradeCode" `
    -d "BinDir=$BinDir" `
    -d "SrcDir=$SrcDir" `
    @OptimizerDefine `
    -arch x64 `
    -o $MsiPath

if ($LASTEXITCODE -ne 0) {
    Write-Error "WiX build failed."
    exit 1
}

# --- Sign MSI (if signtool available) ---
# Azure Trusted Signing: requires azure-sign-tool or signtool with Azure Key Vault
# For now, warn if unsigned. Signing will be wired in once Azure Trusted Signing
# identity validation completes.
$signTool = Get-Command signtool -ErrorAction SilentlyContinue
if ($signTool) {
    Write-Host "TODO: Authenticode signing not yet configured. MSI is unsigned."
    Write-Host "  Azure Trusted Signing must be set up before the MSI can be signed."
} else {
    Write-Host "WARNING: signtool not found. MSI is unsigned."
    Write-Host "  Unsigned MSI will trigger SmartScreen warnings."
}

Write-Host "MSI created: $MsiPath"

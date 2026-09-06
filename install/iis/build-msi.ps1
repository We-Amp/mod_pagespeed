# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build MSI installer for IIS PageSpeed module.
#
# Usage:
#   .\install\iis\build-msi.ps1 [-OutputDir C:\output] [-BinDir bazel-bin\pagespeed\iis]
#
# Prerequisites:
#   - .NET SDK 8+ (for dotnet tool)
#   - WiX Toolset v5.0.2 (installed automatically if missing)
#   - Bazel build of //pagespeed/iis:pagespeed_iis.dll completed

param(
    [string]$OutputDir = (Get-Location).Path,
    [string]$BinDir = "",
    [string]$SrcDir = ""
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

$ProductVersion = "$MAJOR.$MINOR.$BUILD"
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
$WxsPath = Join-Path $ScriptDir "Product.wxs"
$MsiName = "pagespeed-iis-$DisplayVersion-win-x64.msi"
$MsiPath = Join-Path $OutputDir $MsiName

Write-Host "Building MSI: $MsiName"
Write-Host "  ProductVersion: $ProductVersion"
Write-Host "  DisplayVersion: $DisplayVersion"
Write-Host "  UpgradeCode:    $UpgradeCode"
Write-Host "  BinDir:         $BinDir"
Write-Host "  SrcDir:         $SrcDir"

wix build $WxsPath `
    -d "ProductVersion=$ProductVersion" `
    -d "DisplayVersion=$DisplayVersion" `
    -d "UpgradeCode=$UpgradeCode" `
    -d "BinDir=$BinDir" `
    -d "SrcDir=$SrcDir" `
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
    Write-Host "  See the design record Phase 6: Azure Trusted Signing setup required."
} else {
    Write-Host "WARNING: signtool not found. MSI is unsigned."
    Write-Host "  Unsigned MSI will trigger SmartScreen warnings."
}

Write-Host "MSI created: $MsiPath"

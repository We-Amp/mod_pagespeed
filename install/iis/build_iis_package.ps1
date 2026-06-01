# Build a distributable IIS PageSpeed package.
#
# Usage:
#   .\install\iis\build_iis_package.ps1 [-OutputDir C:\output]
#
# Prerequisites:
#   - Bazel build of //pagespeed/iis:pagespeed_iis.dll completed on Windows

param(
    [string]$OutputDir = (Get-Location).Path
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir = (Resolve-Path "$ScriptDir\..\..").Path

# Read version
$VersionFile = Get-Content "$SrcDir\net\instaweb\public\VERSION" -Raw
$VersionFile -split "`n" | ForEach-Object {
    if ($_ -match '^\s*(\w+)\s*=\s*(.+)\s*$') {
        Set-Variable -Name $Matches[1] -Value $Matches[2].Trim()
    }
}

if ($PRERELEASE) {
    $Version = "$MAJOR.$MINOR.$BUILD-$PRERELEASE"
} else {
    $Version = "$MAJOR.$MINOR.$BUILD"
}

$PkgName = "pagespeed-iis-$Version"
$PkgDir = Join-Path $OutputDir $PkgName
$DllPath = Join-Path $SrcDir "bazel-bin\pagespeed\iis\pagespeed_iis.dll"

if (-not (Test-Path $DllPath)) {
    Write-Error "DLL not found at $DllPath. Build it first: bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll"
    exit 1
}

Write-Host "Creating IIS PageSpeed package: $PkgName"

if (Test-Path $PkgDir) { Remove-Item -Recurse -Force $PkgDir }
New-Item -ItemType Directory -Path $PkgDir | Out-Null

# Copy DLL
Copy-Item $DllPath "$PkgDir\pagespeed_iis.dll"

# Create sample web.config
@"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <system.webServer>
    <!-- PageSpeed module is registered globally via New-WebGlobalModule -->
    <!-- Do NOT add it here again or you'll get 500.19 errors -->

    <!-- Required for PageSpeed combined resource URLs containing '+' -->
    <security>
      <requestFiltering allowDoubleEscaping="true" />
    </security>
  </system.webServer>
</configuration>
"@ | Out-File -Encoding UTF8 "$PkgDir\web.config.sample"

# Create known limitations doc
@"
# IIS PageSpeed Known Limitations ($Version)

**Status: Experimental**

## Functional Limitations

- **IPRO cache**: Async cache stubs return misses; Redis/disk caches not functional for IPRO
- **Beacon data**: Silently discarded (no critical CSS/image optimization feedback)
- **Shared memory**: Uses Windows-specific implementation (WindowsSharedMem)
- **External cache**: Redis only (no Memcached support on Windows)

## Configuration

- Configuration via web.config XML (not httpd.conf directives)
- Single IisServerContext per application pool
- allowDoubleEscaping must be enabled for combined resource URLs

## Test Coverage

- C++ unit tests: 19/19 passing (1 build-excluded)
- Python integration tests: 350/356 passing (5 skipped, 1 timing-dependent)

## More Information

See https://github.com/we-amp/mod_pagespeed for details.
"@ | Out-File -Encoding UTF8 "$PkgDir\KNOWN_LIMITATIONS.md"

# Create README
@"
# IIS PageSpeed $Version

Native IIS module for PageSpeed web optimization.

**Status: Experimental**

## Installation

1. Stop IIS:
   ``Stop-Service -Name W3SVC``

2. Copy the DLL:
   ``Copy-Item pagespeed_iis.dll C:\inetpub\pagespeed\ -Force``

3. Register the module:
   ``New-WebGlobalModule -Name PageSpeedModule -Image 'C:\inetpub\pagespeed\pagespeed_iis.dll'``

4. Start IIS:
   ``Start-Service -Name W3SVC``

5. Verify: ``curl -I http://localhost/`` should show ``X-PageSpeed: $Version``

## Requirements

- Windows Server 2019 or later
- IIS 10+
- Visual C++ Redistributable 2022

## Known Limitations

See KNOWN_LIMITATIONS.md for details.

## More Information

- https://github.com/we-amp/mod_pagespeed
"@ | Out-File -Encoding UTF8 "$PkgDir\README.md"

# Create ZIP
$ZipPath = Join-Path $OutputDir "$PkgName-win-x64.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath }
Compress-Archive -Path $PkgDir -DestinationPath $ZipPath
Remove-Item -Recurse -Force $PkgDir

Write-Host "Package created: $ZipPath"

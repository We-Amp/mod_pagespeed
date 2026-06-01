<#
.SYNOPSIS
    Installs PageSpeed IIS module.

.DESCRIPTION
    This script installs the PageSpeed IIS module by:
    1. Copying DLLs to the installation directory
    2. Registering the native module with IIS
    3. Creating the cache directory with proper permissions
    4. Optionally configuring a default license key

.PARAMETER InstallPath
    Installation directory for PageSpeed DLLs.
    Default: C:\Program Files\PageSpeed

.PARAMETER CachePath
    Directory for PageSpeed cache files.
    Default: C:\PageSpeed\cache

.PARAMETER LicenseKey
    Optional license key to configure.

.PARAMETER SkipModuleRegistration
    Skip IIS module registration (for upgrades).

.EXAMPLE
    .\install-pagespeed.ps1

.EXAMPLE
    .\install-pagespeed.ps1 -LicenseKey "XXXX-XXXX-XXXX-XXXX"

.EXAMPLE
    .\install-pagespeed.ps1 -InstallPath "D:\PageSpeed" -CachePath "D:\Cache"
#>

param(
    [string]$InstallPath = "C:\Program Files\PageSpeed",
    [string]$CachePath = "C:\PageSpeed\cache",
    [string]$LicenseKey = "",
    [switch]$SkipModuleRegistration
)

$ErrorActionPreference = "Stop"

# Check for admin privileges
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    exit 1
}

Write-Host "PageSpeed IIS Module Installer" -ForegroundColor Cyan
Write-Host "==============================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Create installation directory
Write-Host "Creating installation directory: $InstallPath" -ForegroundColor Yellow
if (-not (Test-Path $InstallPath)) {
    New-Item -ItemType Directory -Path $InstallPath -Force | Out-Null
}

# Step 2: Copy DLLs
Write-Host "Copying PageSpeed DLLs..." -ForegroundColor Yellow
$sourcePath = Split-Path -Parent $MyInvocation.MyCommand.Path
$dllFiles = @("pagespeed_iis.dll")

foreach ($dll in $dllFiles) {
    $source = Join-Path $sourcePath $dll
    if (Test-Path $source) {
        Copy-Item -Path $source -Destination $InstallPath -Force
        Write-Host "  Copied: $dll" -ForegroundColor Green
    } else {
        Write-Warning "  Not found: $dll (build the module first)"
    }
}

# Step 3: Create cache directory
Write-Host "Creating cache directory: $CachePath" -ForegroundColor Yellow
if (-not (Test-Path $CachePath)) {
    New-Item -ItemType Directory -Path $CachePath -Force | Out-Null
}

# Set permissions for IIS users
Write-Host "Setting cache directory permissions..." -ForegroundColor Yellow
$acl = Get-Acl $CachePath
$rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
    "IIS_IUSRS",
    "FullControl",
    "ContainerInherit,ObjectInherit",
    "None",
    "Allow"
)
$acl.SetAccessRule($rule)
Set-Acl -Path $CachePath -AclObject $acl
Write-Host "  Granted IIS_IUSRS full control" -ForegroundColor Green

# Step 4: Register IIS module
if (-not $SkipModuleRegistration) {
    Write-Host "Registering IIS native module..." -ForegroundColor Yellow

    $modulePath = Join-Path $InstallPath "pagespeed_iis.dll"

    # Import WebAdministration module
    Import-Module WebAdministration -ErrorAction SilentlyContinue

    # Remove existing registration if present
    $existingModule = Get-WebGlobalModule -Name "PageSpeedModule" -ErrorAction SilentlyContinue
    if ($existingModule) {
        Write-Host "  Removing existing module registration..." -ForegroundColor Yellow
        Remove-WebGlobalModule -Name "PageSpeedModule"
    }

    # Register the native module
    New-WebGlobalModule -Name "PageSpeedModule" -Image $modulePath

    # Add to modules collection
    $existingModuleEntry = Get-WebConfigurationProperty -pspath 'MACHINE/WEBROOT/APPHOST' `
        -filter "system.webServer/modules" `
        -name "." | Where-Object { $_.Name -eq "PageSpeedModule" }

    if (-not $existingModuleEntry) {
        Add-WebConfigurationProperty -pspath 'MACHINE/WEBROOT/APPHOST' `
            -filter "system.webServer/modules" `
            -name "." `
            -value @{name='PageSpeedModule'}
    }

    Write-Host "  Module registered successfully" -ForegroundColor Green
}

# Step 5: Create default configuration
Write-Host "Creating default configuration..." -ForegroundColor Yellow
$defaultConfigPath = Join-Path $InstallPath "pagespeed-default.config"
$defaultConfig = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- PageSpeed IIS Module Default Configuration -->
<!-- Copy this to your site's web.config and customize -->
<pageSpeed enabled="true">
  <!-- License key (required for production use) -->
  <license key="$LicenseKey" />

  <!-- Cache configuration -->
  <cache>
    <fileCache path="$CachePath" maxSizeGb="10" />
    <lruCache sizeMb="256" />
  </cache>

  <!-- Enabled filters -->
  <filters>
    <enable name="rewrite_images,combine_css,combine_javascript,collapse_whitespace" />
  </filters>

  <!-- Image optimization -->
  <images>
    <recompression quality="85" />
    <webp enabled="true" quality="80" />
  </images>

  <!-- Admin interface (disable in production or restrict access) -->
  <admin enabled="true" path="/pagespeed_admin" />
  <statistics enabled="true" path="/pagespeed_statistics" />
</pageSpeed>
"@
$defaultConfig | Out-File -FilePath $defaultConfigPath -Encoding utf8
Write-Host "  Created: $defaultConfigPath" -ForegroundColor Green

# Done
Write-Host ""
Write-Host "Installation complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "1. Add PageSpeed configuration to your site's web.config"
Write-Host "2. Restart IIS: iisreset"
Write-Host "3. Visit /pagespeed_admin to verify installation"
Write-Host ""
if ([string]::IsNullOrEmpty($LicenseKey)) {
    Write-Host "Note: No license key configured. Add your license key to web.config" -ForegroundColor Yellow
    Write-Host "for production use." -ForegroundColor Yellow
}

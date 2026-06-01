<#
.SYNOPSIS
    Uninstalls PageSpeed IIS module.

.DESCRIPTION
    This script uninstalls the PageSpeed IIS module by:
    1. Stopping IIS
    2. Unregistering the native module
    3. Removing DLLs from the installation directory
    4. Optionally removing the cache directory

.PARAMETER InstallPath
    Installation directory for PageSpeed DLLs.
    Default: C:\Program Files\PageSpeed

.PARAMETER CachePath
    Directory for PageSpeed cache files.
    Default: C:\PageSpeed\cache

.PARAMETER RemoveCache
    Also remove the cache directory and all cached files.

.PARAMETER KeepConfig
    Keep configuration files.

.EXAMPLE
    .\uninstall-pagespeed.ps1

.EXAMPLE
    .\uninstall-pagespeed.ps1 -RemoveCache
#>

param(
    [string]$InstallPath = "C:\Program Files\PageSpeed",
    [string]$CachePath = "C:\PageSpeed\cache",
    [switch]$RemoveCache,
    [switch]$KeepConfig
)

$ErrorActionPreference = "Stop"

# Check for admin privileges
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    exit 1
}

Write-Host "PageSpeed IIS Module Uninstaller" -ForegroundColor Cyan
Write-Host "================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Stop IIS to release DLL handles
Write-Host "Stopping IIS..." -ForegroundColor Yellow
Stop-Service W3SVC -Force -ErrorAction SilentlyContinue
Stop-Service WAS -Force -ErrorAction SilentlyContinue
Write-Host "  IIS stopped" -ForegroundColor Green

# Step 2: Unregister IIS module
Write-Host "Unregistering IIS module..." -ForegroundColor Yellow

Import-Module WebAdministration -ErrorAction SilentlyContinue

# Remove from modules collection
try {
    $moduleEntry = Get-WebConfigurationProperty -pspath 'MACHINE/WEBROOT/APPHOST' `
        -filter "system.webServer/modules/add[@name='PageSpeedModule']" `
        -name "name" -ErrorAction SilentlyContinue

    if ($moduleEntry) {
        Remove-WebConfigurationProperty -pspath 'MACHINE/WEBROOT/APPHOST' `
            -filter "system.webServer/modules" `
            -name "." `
            -AtElement @{name='PageSpeedModule'}
        Write-Host "  Removed module from modules collection" -ForegroundColor Green
    }
} catch {
    Write-Warning "  Could not remove module entry: $_"
}

# Remove global module registration
try {
    $globalModule = Get-WebGlobalModule -Name "PageSpeedModule" -ErrorAction SilentlyContinue
    if ($globalModule) {
        Remove-WebGlobalModule -Name "PageSpeedModule"
        Write-Host "  Removed global module registration" -ForegroundColor Green
    }
} catch {
    Write-Warning "  Could not remove global module: $_"
}

# Step 3: Remove DLLs
Write-Host "Removing PageSpeed files..." -ForegroundColor Yellow
if (Test-Path $InstallPath) {
    $filesToRemove = @("pagespeed_iis.dll")
    if (-not $KeepConfig) {
        $filesToRemove += @("pagespeed-default.config")
    }

    foreach ($file in $filesToRemove) {
        $filePath = Join-Path $InstallPath $file
        if (Test-Path $filePath) {
            Remove-Item -Path $filePath -Force
            Write-Host "  Removed: $file" -ForegroundColor Green
        }
    }

    # Remove directory if empty
    $remaining = Get-ChildItem $InstallPath -ErrorAction SilentlyContinue
    if (-not $remaining) {
        Remove-Item -Path $InstallPath -Force
        Write-Host "  Removed: $InstallPath" -ForegroundColor Green
    }
}

# Step 4: Remove cache if requested
if ($RemoveCache -and (Test-Path $CachePath)) {
    Write-Host "Removing cache directory..." -ForegroundColor Yellow
    Remove-Item -Path $CachePath -Recurse -Force
    Write-Host "  Removed: $CachePath" -ForegroundColor Green
}

# Step 5: Start IIS
Write-Host "Starting IIS..." -ForegroundColor Yellow
Start-Service WAS -ErrorAction SilentlyContinue
Start-Service W3SVC -ErrorAction SilentlyContinue
Write-Host "  IIS started" -ForegroundColor Green

# Done
Write-Host ""
Write-Host "Uninstallation complete!" -ForegroundColor Green
Write-Host ""
if (-not $RemoveCache -and (Test-Path $CachePath)) {
    Write-Host "Note: Cache directory was preserved at $CachePath" -ForegroundColor Yellow
    Write-Host "Use -RemoveCache to delete it." -ForegroundColor Yellow
}

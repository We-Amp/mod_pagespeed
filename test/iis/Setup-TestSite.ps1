<#
.SYNOPSIS
    Sets up the PageSpeed IIS test site.

.DESCRIPTION
    Creates an IIS site for running PageSpeed integration tests.
    This script should be run as Administrator.

.PARAMETER SiteName
    Name of the IIS site. Default: PageSpeedTest

.PARAMETER Port
    Port for the site. Default: 8080

.PARAMETER SitePath
    Physical path for the site. Default: C:\inetpub\PageSpeedTest

.PARAMETER AppPoolName
    Name of the application pool. Default: PageSpeedTestPool

.EXAMPLE
    .\Setup-TestSite.ps1

.EXAMPLE
    .\Setup-TestSite.ps1 -Port 9000 -SiteName "PSTest"
#>

param(
    [string]$SiteName = "PageSpeedTest",
    [int]$Port = 8080,
    [string]$SitePath = "C:\inetpub\PageSpeedTest",
    [string]$AppPoolName = "PageSpeedTestPool"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$TestSiteDir = Join-Path $ScriptDir "testsite"

# Check for admin privileges
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    exit 1
}

Write-Host "PageSpeed IIS Test Site Setup" -ForegroundColor Cyan
Write-Host "=============================" -ForegroundColor Cyan
Write-Host ""

# Import IIS module
Import-Module WebAdministration -ErrorAction SilentlyContinue
if (-not (Get-Module WebAdministration)) {
    Write-Error "WebAdministration module not found. Is IIS installed?"
    exit 1
}

# Step 1: Create site directory
Write-Host "Creating site directory..." -ForegroundColor Yellow
if (-not (Test-Path $SitePath)) {
    New-Item -ItemType Directory -Path $SitePath -Force | Out-Null
}
Write-Host "  Created: $SitePath" -ForegroundColor Green

# Step 2: Copy test site files
Write-Host "Copying test site files..." -ForegroundColor Yellow
if (Test-Path $TestSiteDir) {
    Copy-Item -Path "$TestSiteDir\*" -Destination $SitePath -Recurse -Force
    Write-Host "  Copied files from: $TestSiteDir" -ForegroundColor Green
} else {
    Write-Host "  Warning: Test site directory not found: $TestSiteDir" -ForegroundColor Yellow
    # Create minimal default document
    @"
<!DOCTYPE html>
<html>
<head><title>PageSpeed Test</title></head>
<body><h1>PageSpeed IIS Test Site</h1></body>
</html>
"@ | Out-File -FilePath (Join-Path $SitePath "index.html") -Encoding utf8
    Write-Host "  Created minimal index.html" -ForegroundColor Green
}

# Step 3: Create application pool
Write-Host "Creating application pool..." -ForegroundColor Yellow
$existingPool = Get-IISAppPool -Name $AppPoolName -ErrorAction SilentlyContinue
if ($existingPool) {
    Write-Host "  App pool '$AppPoolName' already exists" -ForegroundColor Green
} else {
    New-WebAppPool -Name $AppPoolName | Out-Null
    # Configure pool for 64-bit
    Set-ItemProperty "IIS:\AppPools\$AppPoolName" -Name "enable32BitAppOnWin64" -Value $false
    Write-Host "  Created: $AppPoolName" -ForegroundColor Green
}

# Step 4: Create or update website
Write-Host "Creating website..." -ForegroundColor Yellow
$existingSite = Get-Website -Name $SiteName -ErrorAction SilentlyContinue
if ($existingSite) {
    Write-Host "  Site '$SiteName' already exists, updating..." -ForegroundColor Yellow
    Set-ItemProperty "IIS:\Sites\$SiteName" -Name physicalPath -Value $SitePath
    Set-ItemProperty "IIS:\Sites\$SiteName" -Name applicationPool -Value $AppPoolName
} else {
    New-Website -Name $SiteName -PhysicalPath $SitePath -Port $Port -ApplicationPool $AppPoolName | Out-Null
    Write-Host "  Created: $SiteName on port $Port" -ForegroundColor Green
}

# Step 5: Set permissions
Write-Host "Setting permissions..." -ForegroundColor Yellow
$acl = Get-Acl $SitePath
$identities = @("IIS_IUSRS", "IUSR")
foreach ($identity in $identities) {
    try {
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $identity, "ReadAndExecute", "ContainerInherit,ObjectInherit", "None", "Allow"
        )
        $acl.AddAccessRule($rule)
    } catch {
        Write-Host "  Could not add rule for $identity" -ForegroundColor Yellow
    }
}
Set-Acl $SitePath $acl
Write-Host "  Permissions set" -ForegroundColor Green

# Step 6: Enable PageSpeed module for the site (if installed)
Write-Host "Checking PageSpeed module..." -ForegroundColor Yellow
$psModule = Get-WebGlobalModule -Name "PageSpeedModule" -ErrorAction SilentlyContinue
if ($psModule) {
    Write-Host "  PageSpeed module is installed globally" -ForegroundColor Green
} else {
    Write-Host "  PageSpeed module not installed - install it before running tests" -ForegroundColor Yellow
}

# Step 7: Create web.config with IIS settings (no pagespeed XML section)
Write-Host "Creating web.config..." -ForegroundColor Yellow
$webConfig = @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
    <system.webServer>
        <staticContent>
            <mimeMap fileExtension=".css" mimeType="text/css" />
            <mimeMap fileExtension=".js" mimeType="application/javascript" />
        </staticContent>
        <defaultDocument>
            <files>
                <add value="index.html" />
                <add value="default.htm" />
            </files>
        </defaultDocument>
    </system.webServer>
</configuration>
"@
$webConfig | Out-File -FilePath (Join-Path $SitePath "web.config") -Encoding utf8
Write-Host "  Created web.config" -ForegroundColor Green

# Step 7b: Create pagespeed.config flat file (IISpeed format, the design record)
Write-Host "Creating pagespeed.config..." -ForegroundColor Yellow
$pagespeedConfig = @"
pagespeed on
pagespeed RewriteLevel CoreFilters
pagespeed FileCachePath C:\PageSpeed\cache
pagespeed Statistics on
pagespeed StatisticsLogging on
pagespeed EnableCachePurge on
pagespeed RateLimitBackgroundFetches on
pagespeed AdminPath /pagespeed_admin
pagespeed StatisticsPath /pagespeed_statistics
pagespeed GlobalStatisticsPath /pagespeed_global_statistics
pagespeed ConsolePath /pagespeed_console
pagespeed MessagesPath /pagespeed_message
pagespeed GlobalAdminPath /pagespeed_global_admin
"@

# Site-level config
$pagespeedConfig | Out-File -FilePath (Join-Path $SitePath "pagespeed.config") -Encoding utf8
Write-Host "  Created pagespeed.config in site root" -ForegroundColor Green

# Server-level config at %ProgramData%\We-Amp\IISWebSpeed\
$serverConfigDir = "$env:ProgramData\We-Amp\IISWebSpeed"
if (-not (Test-Path $serverConfigDir)) {
    New-Item -ItemType Directory -Path $serverConfigDir -Force | Out-Null
}
$pagespeedConfig | Out-File -FilePath (Join-Path $serverConfigDir "pagespeed.config") -Encoding utf8
Write-Host "  Created pagespeed.config at $serverConfigDir" -ForegroundColor Green

# Step 8: Start the site
Write-Host "Starting site..." -ForegroundColor Yellow
Start-Website -Name $SiteName -ErrorAction SilentlyContinue
Write-Host "  Site started" -ForegroundColor Green

# Done
Write-Host ""
Write-Host "Setup complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Test site URL: http://localhost:$Port/" -ForegroundColor Cyan
Write-Host "Admin URL:     http://localhost:$Port/pagespeed_admin" -ForegroundColor Cyan
Write-Host ""
Write-Host "Run tests with: .\Run-IISTests.ps1 -Port $Port" -ForegroundColor Yellow

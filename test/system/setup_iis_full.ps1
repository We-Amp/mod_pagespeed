# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

<#
.SYNOPSIS
    Setup and manage Full IIS (not IIS Express) for PageSpeed integration tests.

.DESCRIPTION
    This script sets up Full IIS (Windows Server feature) with the PageSpeed native
    module for running integration tests. This is preferred over IIS Express because:
    - Runs as a Windows Service (proper background process)
    - Supports native module registration via GlobalModules
    - More robust process management
    - Works properly over SSH sessions

.PARAMETER Action
    The action to perform: start, stop, status, restart, install, uninstall

.PARAMETER Port
    The port for IIS to listen on (default: 8080)

.PARAMETER SourceRoot
    Path to the mod_pagespeed source root (default: auto-detect)

.PARAMETER NoModule
    Skip installing the PageSpeed module (for baseline testing)

.EXAMPLE
    .\setup_iis_full.ps1 install          # First-time setup
    .\setup_iis_full.ps1 start            # Start IIS (usually already running)
    .\setup_iis_full.ps1 stop             # Stop IIS
    .\setup_iis_full.ps1 status           # Check status
    .\setup_iis_full.ps1 install -NoModule  # Install without PageSpeed module
#>

param(
    [Parameter(Position=0)]
    [ValidateSet('start', 'stop', 'status', 'restart', 'install', 'uninstall')]
    [string]$Action = 'status',

    [int]$Port = 8080,

    [string]$SourceRoot = "",

    [switch]$NoModule
)

$ErrorActionPreference = "Stop"

# Configuration
$SiteName = "PageSpeedTestSite"
$AppPoolName = "PageSpeedTestPool"
$WebRoot = "C:\inetpub\pagespeed_test"
$CacheDir = "C:\pagespeed_cache"
$LogDir = "C:\pagespeed_logs"

# Auto-detect source root if not provided
if (-not $SourceRoot) {
    # Try multiple locations
    $scriptParent = $null
    try {
        $scriptItem = Get-Item $PSScriptRoot -ErrorAction SilentlyContinue
        if ($scriptItem -and $scriptItem.Parent -and $scriptItem.Parent.Parent) {
            $scriptParent = $scriptItem.Parent.Parent.FullName
        }
    } catch { }

    $possibleRoots = @(
        "C:\pagespeed",
        $scriptParent,
        "\\host.lan\Data"
    )
    foreach ($root in $possibleRoots) {
        if ($root -and (Test-Path "$root\install\mod_pagespeed_example")) {
            $SourceRoot = $root
            break
        }
    }
    if (-not $SourceRoot) {
        $SourceRoot = "C:\pagespeed"
    }
}

# Paths to test content
$ExampleSource = "$SourceRoot\install\mod_pagespeed_example"
$TestSource = "$SourceRoot\install\mod_pagespeed_test"
$DoNotModifySource = "$SourceRoot\install\do_not_modify"

# Path to built PageSpeed module
$ModulePath = "$SourceRoot\bazel-bin\pagespeed\iis\pagespeed_iis.dll"

function Write-Status {
    param([string]$Message, [string]$Color = "White")
    Write-Host "[IIS Setup] " -NoNewline -ForegroundColor Cyan
    Write-Host $Message -ForegroundColor $Color
}

function Test-IISInstalled {
    try {
        $feature = Get-WindowsFeature -Name Web-Server -ErrorAction Stop
        return $feature.Installed
    } catch {
        # Not on Server, check for IIS service
        $service = Get-Service -Name W3SVC -ErrorAction SilentlyContinue
        return $null -ne $service
    }
}

function Test-AppCmdExists {
    $appcmd = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
    return Test-Path $appcmd
}

function Invoke-AppCmd {
    # Use $args (automatic variable) instead of a named parameter to capture
    # ALL positional arguments. PowerShell's param([string[]]$X) only binds
    # the first positional value; remaining args are silently dropped.
    $appcmd = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
    if (-not (Test-Path $appcmd)) {
        throw "appcmd.exe not found. Is IIS installed?"
    }
    & $appcmd @args 2>&1
}

function Initialize-TestEnvironment {
    Write-Status "Initializing test environment..."

    # Create directories
    # Include C:\PageSpeed\cache as a safety net — IisConfig defaults to this
    # path if config parsing fails (iis_config.cc:428).
    @($WebRoot, $CacheDir, $LogDir, "C:\PageSpeed", "C:\PageSpeed\cache") | ForEach-Object {
        if (-not (Test-Path $_)) {
            New-Item -ItemType Directory -Path $_ -Force | Out-Null
            Write-Status "Created directory: $_" "Gray"
        }
    }

    # Set permissions on cache and log directories
    # IIS app pools run as IIS AppPool\<pool-name> or as IUSR
    try {
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            "IIS_IUSRS", "FullControl", "ContainerInherit,ObjectInherit", "None", "Allow")

        foreach ($dir in @($CacheDir, $LogDir, "C:\PageSpeed\cache")) {
            $acl = Get-Acl $dir
            $acl.AddAccessRule($rule)
            Set-Acl $dir $acl
        }

        Write-Status "Set IIS_IUSRS permissions on cache/log directories" "Gray"
    } catch {
        Write-Status "Warning: Could not set permissions: $_" "Yellow"
    }

    # Remove stale web.config from previous runs. appcmd set config (called
    # later in New-IISSite) triggers IIS to parse the site's web.config.
    # New-WebConfig writes a fresh one after module installation.
    $staleConfig = "$WebRoot\web.config"
    if (Test-Path $staleConfig) {
        Remove-Item $staleConfig -Force
        Write-Status "Removed stale web.config" "Gray"
    }

    # Also remove stale pagespeed.config from previous runs
    $stalePagespeedConfig = "$WebRoot\pagespeed.config"
    if (Test-Path $stalePagespeedConfig) {
        Remove-Item $stalePagespeedConfig -Force
        Write-Status "Removed stale pagespeed.config" "Gray"
    }

    # Copy test content
    Write-Status "Copying test content..."

    if (Test-Path $ExampleSource) {
        $destExample = "$WebRoot\mod_pagespeed_example"
        if (Test-Path $destExample) { Remove-Item $destExample -Recurse -Force }
        Copy-Item $ExampleSource $destExample -Recurse
        Write-Status "Copied example content to $destExample" "Gray"
    } else {
        Write-Status "Warning: Example source not found at $ExampleSource" "Yellow"
    }

    if (Test-Path $TestSource) {
        $destTest = "$WebRoot\mod_pagespeed_test"
        if (Test-Path $destTest) { Remove-Item $destTest -Recurse -Force }
        Copy-Item $TestSource $destTest -Recurse
        Write-Status "Copied test content to $destTest" "Gray"
    } else {
        Write-Status "Warning: Test source not found at $TestSource" "Yellow"
    }

    if (Test-Path $DoNotModifySource) {
        $destDoNotModify = "$WebRoot\do_not_modify"
        if (Test-Path $destDoNotModify) { Remove-Item $destDoNotModify -Recurse -Force }
        Copy-Item $DoNotModifySource $destDoNotModify -Recurse
        Write-Status "Copied do_not_modify content" "Gray"
    }

    # Create per-directory web.config files to match Apache's debug.conf.
    # Apache sets Cache-Control: no-cache for the no_cache/ directory.
    $noCacheDir = "$WebRoot\mod_pagespeed_test\no_cache"
    if (Test-Path $noCacheDir) {
        @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
  <system.webServer>
    <staticContent>
      <clientCache cacheControlMode="DisableCache" />
    </staticContent>
  </system.webServer>
</configuration>
"@ | Set-Content "$noCacheDir\web.config" -Encoding UTF8
        Write-Status "Created no-cache web.config for $noCacheDir" "Gray"
    }

    # Create a simple index.html for testing
    @"
<!DOCTYPE html>
<html>
<head>
    <title>PageSpeed IIS Test</title>
</head>
<body>
    <h1>PageSpeed IIS Test Server</h1>
    <p>Server is running on port $Port</p>
</body>
</html>
"@ | Set-Content "$WebRoot\index.html"
}

function Install-IISComponents {
    Write-Status "Checking IIS installation..."

    if (-not (Test-IISInstalled)) {
        Write-Status "IIS is not installed. Installing..." "Yellow"
        try {
            # Install IIS with compression support
            # Note: Feature names are Web-Stat-Compression (static) and Web-Dyn-Compression (dynamic)
            Install-WindowsFeature -Name Web-Server,Web-Common-Http,Web-Static-Content,Web-Default-Doc,Web-Http-Errors,Web-Http-Logging,Web-Stat-Compression,Web-Dyn-Compression,Web-Mgmt-Console,Web-Mgmt-Tools -IncludeManagementTools
            Write-Status "IIS installed successfully" "Green"
        } catch {
            throw "Failed to install IIS: $_"
        }
    } else {
        Write-Status "IIS is already installed" "Green"
        # Ensure compression features are installed even if IIS was already present
        try {
            $compressionFeature = Get-WindowsFeature -Name Web-Stat-Compression -ErrorAction SilentlyContinue
            if ($compressionFeature -and -not $compressionFeature.Installed) {
                Write-Status "Installing compression features..." "Yellow"
                Install-WindowsFeature -Name Web-Stat-Compression,Web-Dyn-Compression | Out-Null
            }
        } catch { }
    }

    # Raise http.sys URL segment length limit. Default is 260 (MAX_PATH),
    # which is too short for PageSpeed combined resource URLs that concatenate
    # many filenames into a single path segment (e.g., a+b+c+d.pagespeed.cc.X.css).
    $httpParams = "HKLM:\SYSTEM\CurrentControlSet\Services\HTTP\Parameters"
    $current = Get-ItemProperty -Path $httpParams -Name "UrlSegmentMaxLength" -ErrorAction SilentlyContinue
    if (-not $current -or $current.UrlSegmentMaxLength -lt 4096) {
        Set-ItemProperty -Path $httpParams -Name "UrlSegmentMaxLength" -Value 4096 -Type DWord
        Write-Status "Set http.sys UrlSegmentMaxLength=4096" "Gray"
        # http.sys is a kernel driver — use net stop/start which handles it
        # more reliably than Stop-Service (which can leave it in StopPending).
        & net stop http /y 2>$null
        Start-Sleep -Seconds 2
        & net start http 2>$null
        Write-Status "Restarted HTTP service for registry change" "Gray"
    }

    # Ensure services are running
    Start-Service WAS -ErrorAction SilentlyContinue
    Start-Service W3SVC -ErrorAction SilentlyContinue
}

function New-IISSite {
    Write-Status "Configuring IIS site..."

    # Delete existing site if present
    $existingSite = Invoke-AppCmd list site $SiteName 2>$null
    if ($existingSite) {
        Write-Status "Removing existing site: $SiteName" "Gray"
        Invoke-AppCmd delete site $SiteName | Out-Null
    }

    # Delete existing app pool if present
    $existingPool = Invoke-AppCmd list apppool $AppPoolName 2>$null
    if ($existingPool) {
        Write-Status "Removing existing app pool: $AppPoolName" "Gray"
        Invoke-AppCmd delete apppool $AppPoolName | Out-Null
    }

    # Create app pool with no managed code (for native module only)
    Write-Status "Creating application pool: $AppPoolName" "Gray"
    Invoke-AppCmd add apppool /name:$AppPoolName /managedRuntimeVersion:"" /managedPipelineMode:Integrated | Out-Null

    # Configure app pool settings
    Invoke-AppCmd set apppool $AppPoolName /processModel.identityType:ApplicationPoolIdentity | Out-Null
    Invoke-AppCmd set apppool $AppPoolName /autoStart:true | Out-Null

    # Create site
    Write-Status "Creating site: $SiteName on port $Port" "Gray"
    Invoke-AppCmd add site /name:$SiteName /physicalPath:$WebRoot /bindings:"http/*:${Port}:" | Out-Null

    # Assign app pool to site
    Invoke-AppCmd set site $SiteName /applicationDefaults.applicationPool:$AppPoolName | Out-Null

    # Configure MIME types for modern formats
    Write-Status "Configuring MIME types..." "Gray"
    try {
        Invoke-AppCmd set config $SiteName /section:staticContent /+"[fileExtension='.webp',mimeType='image/webp']" 2>$null
    } catch { }
    try {
        Invoke-AppCmd set config $SiteName /section:staticContent /+"[fileExtension='.woff2',mimeType='font/woff2']" 2>$null
    } catch { }

    # Enable static and dynamic compression for the site
    Write-Status "Configuring compression..." "Gray"
    try {
        Invoke-AppCmd set config $SiteName /section:urlCompression /doStaticCompression:true /doDynamicCompression:true 2>$null
    } catch { }

    Write-Status "IIS site configured successfully" "Green"
}

function New-WebConfig {
    Write-Status "Creating web.config and pagespeed.config..."

    # Write web.config with IIS settings only (no <pagespeed> XML section).
    # The IISpeed-adopted module reads pagespeed.config flat files.
    $webConfig = @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
  <system.webServer>
    <defaultDocument enabled="true">
      <files>
        <clear />
        <add value="index.html" />
      </files>
    </defaultDocument>

    <directoryBrowse enabled="false" />

    <httpErrors errorMode="Detailed" />

    <httpProtocol>
      <customHeaders>
        <add name="X-PageSpeed-Test" value="IIS-Full" />
      </customHeaders>
    </httpProtocol>

    <!-- Required for PageSpeed combined resource URLs containing '+' -->
    <security>
      <requestFiltering allowDoubleEscaping="true">
        <requestLimits maxUrl="16384" maxQueryString="8192" />
      </requestFiltering>
    </security>

  </system.webServer>

</configuration>
"@

    $webConfig | Set-Content "$WebRoot\web.config" -Encoding UTF8
    Write-Status "web.config written to $WebRoot\web.config" "Gray"

    # Generate pagespeed.config flat file (IISpeed format)
    $hasModule = (Test-Path $ModulePath) -and (-not $NoModule)

    if ($hasModule) {
        $pagespeedConfig = @"
pagespeed on
pagespeed RewriteLevel CoreFilters
pagespeed FileCachePath $CacheDir
pagespeed Statistics on
pagespeed StatisticsLogging on
pagespeed EnableCachePurge on
pagespeed RateLimitBackgroundFetches on
pagespeed InPlaceResourceOptimization on
pagespeed CriticalImagesBeaconEnabled false
pagespeed BlockingRewriteKey psatest
pagespeed Library 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js
pagespeed MessageBufferSize 100000
pagespeed AdminPath /pagespeed_admin
pagespeed StatisticsPath /pagespeed_statistics
pagespeed GlobalStatisticsPath /pagespeed_global_statistics
pagespeed ConsolePath /pagespeed_console
pagespeed MessagesPath /pagespeed_message
pagespeed GlobalAdminPath /pagespeed_global_admin
"@

        # Site-level config
        $pagespeedConfig | Set-Content "$WebRoot\pagespeed.config" -Encoding UTF8
        Write-Status "pagespeed.config written to $WebRoot\pagespeed.config" "Gray"

        # Server-level config at %ProgramData%\We-Amp\IISWebSpeed\
        $serverConfigDir = "$env:ProgramData\We-Amp\IISWebSpeed"
        if (-not (Test-Path $serverConfigDir)) {
            New-Item -ItemType Directory -Path $serverConfigDir -Force | Out-Null
            Write-Status "Created server config directory: $serverConfigDir" "Gray"
        }
        $pagespeedConfig | Set-Content "$serverConfigDir\pagespeed.config" -Encoding UTF8
        Write-Status "pagespeed.config written to $serverConfigDir\pagespeed.config" "Gray"

        Write-Status "PageSpeed module will be enabled (flat-file config)" "Cyan"
    } else {
        if ($NoModule) {
            Write-Status "PageSpeed module disabled by -NoModule flag" "Yellow"
        } else {
            Write-Status "PageSpeed module not found at $ModulePath" "Yellow"
            Write-Status "Tests will run without PageSpeed optimizations" "Yellow"
        }
    }
}
function Install-PageSpeedModule {
    if ($NoModule) {
        Write-Status "Skipping PageSpeed module installation (-NoModule)" "Yellow"
        return
    }

    if (-not (Test-Path $ModulePath)) {
        Write-Status "PageSpeed module not found at: $ModulePath" "Yellow"
        Write-Status "Build it with: bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis_dll" "Gray"
        return
    }

    Write-Status "Installing PageSpeed native module..."

    # Uninstall existing module first to release the DLL lock, then stop
    # IIS so w3wp unloads the DLL before we overwrite it.
    try { Invoke-AppCmd uninstall module PageSpeedModule 2>$null } catch { }
    try { Invoke-AppCmd delete module PageSpeedModule 2>$null } catch { }
    Stop-Service W3SVC -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    # Copy DLL to system location
    $systemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"
    Copy-Item $ModulePath $systemModulePath -Force
    Write-Status "Copied module to $systemModulePath" "Gray"

    # Restart IIS services
    Start-Service WAS -ErrorAction SilentlyContinue
    Start-Service W3SVC -ErrorAction SilentlyContinue

    try {
        # Install as global native module (/add:true enables it for all sites)
        Invoke-AppCmd install module /name:PageSpeedModule /image:$systemModulePath /add:true
        Write-Status "Registered PageSpeedModule as global module" "Green"
    } catch {
        # If install fails (e.g. already exists), try just adding it
        try {
            Invoke-AppCmd add module /name:PageSpeedModule /type: /preCondition:
            Write-Status "Added PageSpeedModule (was already installed)" "Yellow"
        } catch {
            Write-Status "Warning: Could not register module: $_" "Yellow"
        }
    }
}

function Uninstall-PageSpeedModule {
    Write-Status "Uninstalling PageSpeed module..."

    try {
        Invoke-AppCmd uninstall module PageSpeedModule 2>$null
        Write-Status "Unregistered PageSpeedModule" "Gray"
    } catch { }


    $systemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"
    if (Test-Path $systemModulePath) {
        Remove-Item $systemModulePath -Force -ErrorAction SilentlyContinue
        Write-Status "Removed $systemModulePath" "Gray"
    }
}

function Start-IISSite {
    Write-Status "Starting IIS..."

    # Ensure services are running
    Start-Service WAS -ErrorAction SilentlyContinue
    Start-Service W3SVC -ErrorAction SilentlyContinue

    # Start app pool
    try {
        Invoke-AppCmd start apppool $AppPoolName | Out-Null
    } catch { }

    # Start site
    try {
        Invoke-AppCmd start site $SiteName | Out-Null
    } catch { }

    # Wait for server to be ready
    Write-Status "Waiting for server to be ready..."
    $maxWait = 30
    $waited = 0
    while ($waited -lt $maxWait) {
        Start-Sleep -Seconds 1
        $waited++

        try {
            $response = Invoke-WebRequest -Uri "http://localhost:$Port/" -UseBasicParsing -TimeoutSec 2 -ErrorAction SilentlyContinue
            if ($response.StatusCode -eq 200) {
                Write-Status "Server is ready on port $Port!" "Green"
                break
            }
        } catch {
            # Server not ready yet
        }
    }

    if ($waited -ge $maxWait) {
        Write-Status "Timeout waiting for server to start" "Red"
        Get-ServerStatus
        exit 1
    }

    # Display environment variables for tests
    Write-Status ""
    Write-Status "Test environment ready. Set these environment variables:" "Cyan"
    Write-Status '  $env:PAGESPEED_HOST = "localhost"' "White"
    Write-Status "  `$env:PAGESPEED_PORT = '$Port'" "White"
    Write-Status '  $env:PAGESPEED_TEST_ROOT = "/mod_pagespeed_test"' "White"
    Write-Status '  $env:PAGESPEED_EXAMPLE_ROOT = "/mod_pagespeed_example"' "White"
    Write-Status "  `$env:PAGESPEED_CACHE_DIR = '$CacheDir'" "White"
    Write-Status '  $env:PAGESPEED_SERVER_TYPE = "iis"' "White"

    $hasModule = (Test-Path $ModulePath) -and (-not $NoModule)
    if ($hasModule) {
        Write-Status '  $env:PAGESPEED_STATS_ENABLED = "1"' "White"
    } else {
        Write-Status '  $env:PAGESPEED_STATS_ENABLED = "0"' "White"
    }
}

function Stop-IISSite {
    Write-Status "Stopping IIS site..."

    try {
        Invoke-AppCmd stop site $SiteName 2>$null | Out-Null
        Write-Status "Stopped site: $SiteName" "Gray"
    } catch { }

    try {
        Invoke-AppCmd stop apppool $AppPoolName 2>$null | Out-Null
        Write-Status "Stopped app pool: $AppPoolName" "Gray"
    } catch { }

    Write-Status "IIS site stopped" "Green"
}

function Get-ServerStatus {
    Write-Status "IIS Status:"

    # Check services
    $w3svc = Get-Service -Name W3SVC -ErrorAction SilentlyContinue
    if ($w3svc) {
        $color = if ($w3svc.Status -eq 'Running') { "Green" } else { "Yellow" }
        Write-Status "  W3SVC service: $($w3svc.Status)" $color
    } else {
        Write-Status "  W3SVC service: Not installed" "Red"
        return
    }

    # Check app pool
    try {
        $poolStatus = Invoke-AppCmd list apppool $AppPoolName
        if ($poolStatus) {
            $state = if ($poolStatus -match "state:(\w+)") { $matches[1] } else { "Unknown" }
            $color = if ($state -eq "Started") { "Green" } else { "Yellow" }
            Write-Status "  App pool '$AppPoolName': $state" $color
        } else {
            Write-Status "  App pool '$AppPoolName': Not found" "Yellow"
        }
    } catch {
        Write-Status "  App pool '$AppPoolName': Error checking status" "Yellow"
    }

    # Check site
    try {
        $siteStatus = Invoke-AppCmd list site $SiteName
        if ($siteStatus) {
            $state = if ($siteStatus -match "state:(\w+)") { $matches[1] } else { "Unknown" }
            $color = if ($state -eq "Started") { "Green" } else { "Yellow" }
            Write-Status "  Site '$SiteName': $state" $color
        } else {
            Write-Status "  Site '$SiteName': Not found" "Yellow"
        }
    } catch {
        Write-Status "  Site '$SiteName': Error checking status" "Yellow"
    }

    # Try to connect
    try {
        $response = Invoke-WebRequest -Uri "http://localhost:$Port/" -UseBasicParsing -TimeoutSec 5 -ErrorAction SilentlyContinue
        Write-Status "  HTTP response: $($response.StatusCode)" "Green"

        # Check for PageSpeed headers
        $psHeader = $response.Headers["X-Mod-Pagespeed"]
        if ($psHeader) {
            Write-Status "  PageSpeed version: $psHeader" "Cyan"
        } else {
            Write-Status "  PageSpeed module: Not active (no X-Mod-Pagespeed header)" "Yellow"
        }

        $testHeader = $response.Headers["X-PageSpeed-Test"]
        if ($testHeader) {
            Write-Status "  Test header: $testHeader" "Gray"
        }
    } catch {
        Write-Status "  HTTP response: Cannot connect to port $Port" "Red"
    }

    # Check module registration
    Write-Status ""
    Write-Status "Module status:"
    try {
        $modules = Invoke-AppCmd list module 2>$null
        $hasPageSpeed = $modules | Where-Object { $_ -match "PageSpeedModule" }
        if ($hasPageSpeed) {
            Write-Status "  PageSpeedModule: Registered" "Green"
        } else {
            Write-Status "  PageSpeedModule: Not registered" "Yellow"
        }
    } catch {
        Write-Status "  Could not check module status" "Yellow"
    }

    # Check DLL
    $systemModulePath = "C:\Windows\System32\inetsrv\pagespeed_iis.dll"
    if (Test-Path $systemModulePath) {
        Write-Status "  Module DLL: $systemModulePath exists" "Green"
    } elseif (Test-Path $ModulePath) {
        Write-Status "  Module DLL: Built at $ModulePath (not installed)" "Yellow"
    } else {
        Write-Status "  Module DLL: Not found" "Yellow"
    }
}

function Uninstall-IISSite {
    Write-Status "Uninstalling test site..."

    Stop-IISSite

    # Remove site
    try {
        Invoke-AppCmd delete site $SiteName 2>$null | Out-Null
        Write-Status "Deleted site: $SiteName" "Gray"
    } catch { }

    # Remove app pool
    try {
        Invoke-AppCmd delete apppool $AppPoolName 2>$null | Out-Null
        Write-Status "Deleted app pool: $AppPoolName" "Gray"
    } catch { }

    # Uninstall module
    Uninstall-PageSpeedModule

    Write-Status "Test site uninstalled" "Green"
}

function Install-IISSiteComplete {
    Install-IISComponents
    Initialize-TestEnvironment
    New-IISSite
    Install-PageSpeedModule
    New-WebConfig
    Start-IISSite
}

# Main entry point
switch ($Action) {
    'install' {
        Install-IISSiteComplete
    }
    'uninstall' {
        Uninstall-IISSite
    }
    'start' {
        Start-IISSite
    }
    'stop' {
        Stop-IISSite
    }
    'status' {
        Get-ServerStatus
    }
    'restart' {
        Stop-IISSite
        Start-Sleep -Seconds 2
        Start-IISSite
    }
}

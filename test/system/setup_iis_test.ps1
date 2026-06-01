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
    Setup and manage IIS Express for PageSpeed integration tests.

.DESCRIPTION
    This script sets up IIS Express with the PageSpeed native module for running
    integration tests. It mirrors the functionality of setup_apache_test.sh and
    setup_envoy_test.sh for Windows/IIS.

.PARAMETER Action
    The action to perform: start, stop, status, restart

.PARAMETER Port
    The port for IIS Express to listen on (default: 8080)

.PARAMETER SourceRoot
    Path to the mod_pagespeed source root (default: auto-detect)

.EXAMPLE
    .\setup_iis_test.ps1 start
    .\setup_iis_test.ps1 stop
    .\setup_iis_test.ps1 status
#>

param(
    [Parameter(Position=0)]
    [ValidateSet('start', 'stop', 'status', 'restart')]
    [string]$Action = 'status',

    [int]$Port = 8080,

    [string]$SourceRoot = ""
)

$ErrorActionPreference = "Stop"

# Configuration
$TestRoot = "$env:TEMP\pagespeed_iis_test"
$WebRoot = "$TestRoot\www"
$CacheDir = "$TestRoot\cache"
$LogDir = "$TestRoot\logs"
$ConfigFile = "$TestRoot\applicationhost.config"
$PidFile = "$TestRoot\iisexpress.pid"

# Auto-detect source root if not provided
if (-not $SourceRoot) {
    $SourceRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
}

# Paths to test content
$ExampleSource = "$SourceRoot\install\mod_pagespeed_example"
$TestSource = "$SourceRoot\install\mod_pagespeed_test"
$DoNotModifySource = "$SourceRoot\install\do_not_modify"

# Path to built PageSpeed module
$ModulePath = "$SourceRoot\bazel-bin\pagespeed\iis\pagespeed_iis.dll"

function Write-Status {
    param([string]$Message, [string]$Color = "White")
    Write-Host "[PageSpeed IIS Test] " -NoNewline -ForegroundColor Cyan
    Write-Host $Message -ForegroundColor $Color
}

function Test-IISExpressInstalled {
    $iisExpressPath = "${env:ProgramFiles}\IIS Express\iisexpress.exe"
    $iisExpressPath32 = "${env:ProgramFiles(x86)}\IIS Express\iisexpress.exe"

    if (Test-Path $iisExpressPath) {
        return $iisExpressPath
    }
    if (Test-Path $iisExpressPath32) {
        return $iisExpressPath32
    }
    return $null
}

function Get-IISExpressProcess {
    Get-Process -Name "iisexpress" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*$ConfigFile*" -or $_.Id -eq (Get-Content $PidFile -ErrorAction SilentlyContinue) }
}

function Initialize-TestEnvironment {
    Write-Status "Initializing test environment..."

    # Create directories
    @($TestRoot, $WebRoot, $CacheDir, $LogDir) | ForEach-Object {
        if (-not (Test-Path $_)) {
            New-Item -ItemType Directory -Path $_ -Force | Out-Null
            Write-Status "Created directory: $_" "Gray"
        }
    }

    # Copy test content
    Write-Status "Copying test content..."

    if (Test-Path $ExampleSource) {
        $destExample = "$WebRoot\mod_pagespeed_example"
        if (Test-Path $destExample) { Remove-Item $destExample -Recurse -Force }
        Copy-Item $ExampleSource $destExample -Recurse
        Write-Status "Copied example content" "Gray"
    } else {
        Write-Status "Warning: Example source not found at $ExampleSource" "Yellow"
    }

    if (Test-Path $TestSource) {
        $destTest = "$WebRoot\mod_pagespeed_test"
        if (Test-Path $destTest) { Remove-Item $destTest -Recurse -Force }
        Copy-Item $TestSource $destTest -Recurse
        Write-Status "Copied test content" "Gray"
    } else {
        Write-Status "Warning: Test source not found at $TestSource" "Yellow"
    }

    if (Test-Path $DoNotModifySource) {
        $destDoNotModify = "$WebRoot\do_not_modify"
        if (Test-Path $destDoNotModify) { Remove-Item $destDoNotModify -Recurse -Force }
        Copy-Item $DoNotModifySource $destDoNotModify -Recurse
        Write-Status "Copied do_not_modify content" "Gray"
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

function New-IISExpressConfig {
    Write-Status "Generating IIS Express configuration..."

    # Determine module path - use built DLL or placeholder for testing
    $moduleDll = $ModulePath
    $hasModule = Test-Path $moduleDll
    if (-not $hasModule) {
        Write-Status "Warning: PageSpeed module not found at $moduleDll" "Yellow"
        Write-Status "Tests will run without PageSpeed optimizations" "Yellow"
        $moduleSection = ""
        $pagespeedConfigSection = ""
        $pagespeedSettings = ""
    } else {
        $moduleSection = @"
        <add name="PageSpeedModule" image="$moduleDll" />
"@
        $pagespeedConfigSection = @"
            <sectionGroup name="pagespeed">
                <section name="settings" overrideModeDefault="Allow" />
            </sectionGroup>
"@
        $pagespeedSettings = @"
        <!-- PageSpeed configuration -->
        <pagespeed>
            <settings
                enabled="true"
                fileCachePath="$($CacheDir.Replace('\', '\\'))"
                logPath="$($LogDir.Replace('\', '\\'))"
                rewriteLevel="CoreFilters"
                statisticsEnabled="true"
                adminEnabled="true"
                adminPath="/pagespeed_admin">
                <filters enabledFilters="collapse_whitespace,combine_css,combine_javascript,extend_cache,inline_css,inline_javascript,rewrite_css,rewrite_images,rewrite_javascript" />
                <images recompressQuality="85" webpQuality="80" jpegQuality="85" progressiveJpeg="true" />
                <javascript libraries="43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js" />
                <cache lruCacheSizeBytes="67108864" httpCacheCompressionLevel="9" />
            </settings>
        </pagespeed>
"@
    }

    # Generate applicationhost.config for IIS Express
    $config = @"
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
    <configSections>
        <sectionGroup name="system.applicationHost">
            <section name="applicationPools" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
            <section name="sites" allowDefinition="AppHostOnly" overrideModeDefault="Deny" />
        </sectionGroup>
        <sectionGroup name="system.webServer">
            <section name="defaultDocument" overrideModeDefault="Allow" />
            <section name="directoryBrowse" overrideModeDefault="Allow" />
            <section name="handlers" overrideModeDefault="Allow" />
            <section name="httpCompression" overrideModeDefault="Allow" />
            <section name="httpErrors" overrideModeDefault="Allow" />
            <section name="httpProtocol" overrideModeDefault="Allow" />
            <section name="modules" allowDefinition="MachineToApplication" overrideModeDefault="Allow" />
            <section name="security" overrideModeDefault="Allow" />
            <section name="staticContent" overrideModeDefault="Allow" />
            <section name="urlCompression" overrideModeDefault="Allow" />
$pagespeedConfigSection
        </sectionGroup>
    </configSections>

    <system.applicationHost>
        <applicationPools>
            <add name="PageSpeedTestPool" managedRuntimeVersion="" managedPipelineMode="Integrated" />
        </applicationPools>
        <sites>
            <site name="PageSpeedTestSite" id="1">
                <application path="/" applicationPool="PageSpeedTestPool">
                    <virtualDirectory path="/" physicalPath="$($WebRoot.Replace('\', '\\'))" />
                </application>
                <bindings>
                    <binding protocol="http" bindingInformation="*:${Port}:localhost" />
                </bindings>
            </site>
        </sites>
    </system.applicationHost>

    <system.webServer>
        <defaultDocument enabled="true">
            <files>
                <add value="index.html" />
            </files>
        </defaultDocument>

        <directoryBrowse enabled="false" />

        <urlCompression doStaticCompression="true" doDynamicCompression="true" />

        <staticContent>
            <mimeMap fileExtension=".webp" mimeType="image/webp" />
            <mimeMap fileExtension=".woff2" mimeType="font/woff2" />
        </staticContent>

        <modules>
            <!-- PageSpeed module enabled via globalModules native registration -->
        </modules>

        <globalModules>
$moduleSection
        </globalModules>

$pagespeedSettings

        <httpErrors errorMode="Detailed" />

        <httpProtocol>
            <customHeaders>
                <add name="X-PageSpeed-Test" value="IISExpress" />
            </customHeaders>
        </httpProtocol>
    </system.webServer>
</configuration>
"@

    $config | Set-Content $ConfigFile -Encoding UTF8
    Write-Status "Configuration written to $ConfigFile" "Gray"
}

function Start-IISExpressServer {
    $iisExpress = Test-IISExpressInstalled
    if (-not $iisExpress) {
        Write-Status "IIS Express is not installed!" "Red"
        Write-Status "Install it from: https://www.microsoft.com/en-us/download/details.aspx?id=48264" "Yellow"
        exit 1
    }

    # Check if already running
    $existing = Get-IISExpressProcess
    if ($existing) {
        Write-Status "IIS Express is already running (PID: $($existing.Id))" "Yellow"
        return
    }

    # Initialize environment and config
    Initialize-TestEnvironment
    New-IISExpressConfig

    # Create compression temp directory
    $compressionDir = "$TestRoot\iis_temp\compression"
    if (-not (Test-Path $compressionDir)) {
        New-Item -ItemType Directory -Path $compressionDir -Force | Out-Null
    }

    Write-Status "Starting IIS Express on port $Port..."

    # Detect Session 0 (Windows services) -- IIS Express does not work in Session 0
    # because it requires an interactive user session for its networking stack.
    $sessionId = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
    if ($sessionId -eq 0) {
        Write-Status "WARNING: Running in Session 0 (service context). IIS Express may not work." "Yellow"
        Write-Status "Consider using Full IIS (setup_iis_full.ps1) instead, which runs as a Windows Service." "Yellow"
    }

    # Log file for IIS Express stdout/stderr (avoids buffer deadlock)
    $stdoutLog = "$LogDir\iisexpress_stdout.log"
    $stderrLog = "$LogDir\iisexpress_stderr.log"

    # Start IIS Express
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $iisExpress
    $startInfo.Arguments = "/config:`"$ConfigFile`" /site:PageSpeedTestSite /trace:error"
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.Id | Set-Content $PidFile

    # Start async readers to prevent buffer deadlock (classic .NET Process antipattern).
    # When stdout/stderr buffers fill and nobody reads them, the child process blocks and exits.
    $process.BeginOutputReadLine()
    $process.BeginErrorReadLine()

    # Capture output to log files for diagnostics.
    # Use script-scoped variables so the event handler script blocks can access them.
    $script:iisOutputLines = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
    $script:iisErrorLines = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
    Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { $script:iisOutputLines.Add($EventArgs.Data) }
    } | Out-Null
    Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { $script:iisErrorLines.Add($EventArgs.Data) }
    } | Out-Null

    Write-Status "IIS Express started with PID: $($process.Id) (Session: $sessionId)" "Green"

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
                Write-Status "Server is ready!" "Green"
                break
            }
        } catch {
            # Server not ready yet
        }

        # Check if process is still running
        if ($process.HasExited) {
            Write-Status "IIS Express exited unexpectedly! (exit code: $($process.ExitCode))" "Red"
            Write-Status "Check logs at: $LogDir" "Yellow"
            # Dump captured stdout/stderr for diagnostics
            Start-Sleep -Milliseconds 500  # Allow async readers to flush
            if ($script:iisOutputLines.Count -gt 0) {
                Write-Status "=== IIS Express stdout ===" "Yellow"
                $script:iisOutputLines | ForEach-Object { Write-Status "  $_" "Gray" }
                ($script:iisOutputLines -join "`n") | Set-Content $stdoutLog
            }
            if ($script:iisErrorLines.Count -gt 0) {
                Write-Status "=== IIS Express stderr ===" "Yellow"
                $script:iisErrorLines | ForEach-Object { Write-Status "  $_" "Gray" }
                ($script:iisErrorLines -join "`n") | Set-Content $stderrLog
            }
            exit 1
        }
    }

    if ($waited -ge $maxWait) {
        Write-Status "Timeout waiting for server to start" "Red"
        exit 1
    }

    # Display environment variables for tests
    Write-Status ""
    Write-Status "Test environment ready. Set these environment variables:" "Cyan"
    Write-Status "  `$env:PAGESPEED_HOST = 'localhost'" "White"
    Write-Status "  `$env:PAGESPEED_PORT = '$Port'" "White"
    Write-Status "  `$env:PAGESPEED_TEST_ROOT = '/mod_pagespeed_test'" "White"
    Write-Status "  `$env:PAGESPEED_EXAMPLE_ROOT = '/mod_pagespeed_example'" "White"
    Write-Status "  `$env:PAGESPEED_CACHE_DIR = '$CacheDir'" "White"
    Write-Status "  `$env:PAGESPEED_STATS_ENABLED = '1'" "White"
}

function Stop-IISExpressServer {
    Write-Status "Stopping IIS Express..."

    # Try to get process from PID file
    # Note: Do NOT use $pid -- it is a read-only automatic variable in PowerShell.
    if (Test-Path $PidFile) {
        $savedPid = Get-Content $PidFile
        $process = Get-Process -Id $savedPid -ErrorAction SilentlyContinue
        if ($process) {
            $process | Stop-Process -Force
            Write-Status "Stopped IIS Express (PID: $savedPid)" "Green"
        }
        Remove-Item $PidFile -Force
    }

    # Also stop any other IIS Express processes for our config
    Get-Process -Name "iisexpress" -ErrorAction SilentlyContinue | ForEach-Object {
        $_ | Stop-Process -Force
        Write-Status "Stopped additional IIS Express process (PID: $($_.Id))" "Gray"
    }
}

function Get-ServerStatus {
    $process = Get-IISExpressProcess
    if ($process) {
        Write-Status "IIS Express is running (PID: $($process.Id))" "Green"

        # Try to get response
        try {
            $response = Invoke-WebRequest -Uri "http://localhost:$Port/" -UseBasicParsing -TimeoutSec 5 -ErrorAction SilentlyContinue
            Write-Status "Server responding on port $Port (Status: $($response.StatusCode))" "Green"

            # Check for PageSpeed headers
            $psHeader = $response.Headers["X-Mod-Pagespeed"]
            if ($psHeader) {
                Write-Status "PageSpeed version: $psHeader" "Cyan"
            } else {
                Write-Status "PageSpeed module not active (no X-Mod-Pagespeed header)" "Yellow"
            }
        } catch {
            Write-Status "Server not responding on port $Port" "Yellow"
        }
    } else {
        Write-Status "IIS Express is not running" "Yellow"
    }
}

# Main entry point
switch ($Action) {
    'start' {
        Start-IISExpressServer
    }
    'stop' {
        Stop-IISExpressServer
    }
    'status' {
        Get-ServerStatus
    }
    'restart' {
        Stop-IISExpressServer
        Start-Sleep -Seconds 2
        Start-IISExpressServer
    }
}

<#
.SYNOPSIS
    Runs IIS PageSpeed integration tests.

.DESCRIPTION
    This script sets up and runs Python integration tests against an IIS
    or IIS Express instance with PageSpeed module installed.

    The tests are organized in two categories:
    1. IIS-specific tests (test/iis/) - Tests specific to IIS integration
    2. Shared automatic tests (test/system/automatic/) - Cross-platform tests

.PARAMETER ServerHost
    The hostname of the IIS server. Default: localhost

.PARAMETER Port
    The port of the IIS server. Default: 8080 for IIS Express, 80 for IIS

.PARAMETER UseHttps
    Use HTTPS for connections.

.PARAMETER UseIISExpress
    Use IIS Express instead of full IIS. This starts IIS Express automatically.

.PARAMETER InstallSite
    Install the test site to IIS before running tests.

.PARAMETER SiteName
    Name of the IIS site for the test site. Default: PageSpeedTest

.PARAMETER SkipInstall
    Skip installing pytest dependencies.

.PARAMETER SharedTestsOnly
    Only run shared tests from test/system/automatic/ (skip IIS-specific tests).

.PARAMETER IISTestsOnly
    Only run IIS-specific tests from test/iis/ (skip shared tests).

.PARAMETER Filter
    Pytest filter expression (passed as -k).

.PARAMETER Markers
    Pytest marker expression (passed as -m).

.PARAMETER Verbose
    Enable verbose output.

.EXAMPLE
    .\Run-IISTests.ps1 -UseIISExpress
    # Start IIS Express and run all tests

.EXAMPLE
    .\Run-IISTests.ps1 -InstallSite -Verbose
    # Install test site to full IIS and run tests

.EXAMPLE
    .\Run-IISTests.ps1 -SharedTestsOnly -Filter "combine"
    # Run only the shared combiner tests

.EXAMPLE
    .\Run-IISTests.ps1 -IISTestsOnly -Markers "admin"
    # Run only IIS-specific admin tests
#>

param(
    [string]$ServerHost = "localhost",  # Renamed from $Host to avoid PowerShell reserved variable
    [int]$Port = 0,  # 0 means auto-detect: 8080 for IIS Express, 80 for IIS
    [switch]$UseHttps,
    [switch]$UseIISExpress,
    [switch]$InstallSite,
    [string]$SiteName = "PageSpeedTest",
    [switch]$SkipInstall,
    [switch]$SharedTestsOnly,
    [switch]$IISTestsOnly,
    [string]$Filter = "",
    [string]$Markers = "",
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$TestSiteDir = Join-Path $ScriptDir "testsite"
$SharedTestDir = Join-Path $RepoRoot "test" "system" "automatic"

# Auto-detect port
if ($Port -eq 0) {
    $Port = if ($UseIISExpress) { 8080 } else { 80 }
}

# Colors for output
function Write-Info { Write-Host "[INFO] $args" -ForegroundColor Green }
function Write-Warn { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Write-Err { Write-Host "[ERROR] $args" -ForegroundColor Red }
function Write-Step { Write-Host "[STEP] $args" -ForegroundColor Cyan }

# Check for Python
function Test-Python {
    try {
        $version = & python --version 2>&1
        Write-Info "Found Python: $version"
        return $true
    } catch {
        try {
            $version = & python3 --version 2>&1
            Write-Info "Found Python3: $version"
            return $true
        } catch {
            return $false
        }
    }
}

# Install pytest dependencies
function Install-Dependencies {
    Write-Step "Installing pytest dependencies..."

    try {
        & pip install --user pytest requests
        Write-Info "Dependencies installed"
    } catch {
        try {
            & pip3 install --user pytest requests
            Write-Info "Dependencies installed"
        } catch {
            Write-Err "Failed to install dependencies: $_"
            exit 1
        }
    }
}

# Install test site to IIS
function Install-TestSite {
    Write-Step "Installing test site to IIS..."

    Import-Module WebAdministration -ErrorAction SilentlyContinue

    $sitePath = "C:\inetpub\$SiteName"

    # Create site directory
    if (-not (Test-Path $sitePath)) {
        New-Item -ItemType Directory -Path $sitePath -Force | Out-Null
    }

    # Copy test site files
    Copy-Item -Path "$TestSiteDir\*" -Destination $sitePath -Recurse -Force
    Write-Info "Copied test site to $sitePath"

    # Check if site exists
    $existingSite = Get-Website -Name $SiteName -ErrorAction SilentlyContinue
    if ($existingSite) {
        Write-Info "Site '$SiteName' already exists"
    } else {
        # Create new site
        $binding = if ($UseHttps) { "*:${Port}:" } else { "*:${Port}:" }
        New-Website -Name $SiteName -PhysicalPath $sitePath -Port $Port -Force | Out-Null
        Write-Info "Created site '$SiteName' on port $Port"
    }

    # Grant IIS_IUSRS read access
    $acl = Get-Acl $sitePath
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        "IIS_IUSRS", "ReadAndExecute", "ContainerInherit,ObjectInherit", "None", "Allow"
    )
    $acl.SetAccessRule($rule)
    Set-Acl $sitePath $acl
    Write-Info "Set permissions on $sitePath"

    # Start the site
    Start-Website -Name $SiteName -ErrorAction SilentlyContinue
    Write-Info "Test site '$SiteName' is running"
}

# Wait for IIS to be ready
function Wait-ForIIS {
    Write-Step "Waiting for IIS to be ready..."

    $scheme = if ($UseHttps) { "https" } else { "http" }
    $url = "${scheme}://${ServerHost}:${Port}/"

    $maxAttempts = 30
    $attempt = 0
    $ready = $false

    while (-not $ready -and $attempt -lt $maxAttempts) {
        $attempt++
        try {
            $response = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 2
            if ($response.StatusCode -eq 200) {
                $ready = $true
                Write-Info "IIS is ready (attempt $attempt)"
            }
        } catch {
            Write-Host "." -NoNewline
            Start-Sleep -Seconds 1
        }
    }

    if (-not $ready) {
        Write-Err "IIS did not become ready after $maxAttempts attempts"
        exit 1
    }
}

# Start IIS Express
function Start-IISExpressServer {
    Write-Step "Starting IIS Express..."

    # Run the setup script
    $setupScript = Join-Path $ScriptDir "Setup-IISExpress.ps1"
    if (Test-Path $setupScript) {
        & $setupScript -Port $Port
    }

    # Start IIS Express
    $startScript = Join-Path $ScriptDir "Start-IISExpress.ps1"
    if (Test-Path $startScript) {
        & $startScript -Port $Port -Background
        Write-Info "IIS Express started on port $Port"
    } else {
        Write-Err "Start-IISExpress.ps1 not found"
        exit 1
    }
}

# Stop IIS Express
function Stop-IISExpressServer {
    Write-Step "Stopping IIS Express..."

    $stopScript = Join-Path $ScriptDir "Stop-IISExpress.ps1"
    if (Test-Path $stopScript) {
        & $stopScript
    }
}

# Run pytest
function Run-Tests {
    Write-Step "Running integration tests..."

    # Set environment variables
    $env:PAGESPEED_HOST = $ServerHost
    $env:PAGESPEED_PORT = $Port
    $env:PAGESPEED_HTTPS = if ($UseHttps) { "1" } else { "0" }
    $env:IIS_EXPRESS = if ($UseIISExpress) { "1" } else { "0" }
    # Use empty string (not "/") to avoid double-slash in URLs
    $env:PAGESPEED_TEST_ROOT = ""
    $env:PAGESPEED_EXAMPLE_ROOT = ""
    $env:PAGESPEED_STATS_ENABLED = "1"

    # Determine which test directories to run
    $testDirs = @()
    if (-not $SharedTestsOnly) {
        $testDirs += $ScriptDir
    }
    if (-not $IISTestsOnly -and (Test-Path $SharedTestDir)) {
        $testDirs += $SharedTestDir
    }

    if ($testDirs.Count -eq 0) {
        Write-Err "No test directories selected"
        exit 1
    }

    # Build pytest command
    $pytestArgs = @()
    $pytestArgs += $testDirs
    $pytestArgs += "--confcutdir=$ScriptDir"

    if ($Verbose) {
        $pytestArgs += "-v"
    }

    if ($Filter) {
        $pytestArgs += "-k"
        $pytestArgs += $Filter
    }

    if ($Markers) {
        $pytestArgs += "-m"
        $pytestArgs += $Markers
    }

    Write-Info "Running: python -m pytest $($pytestArgs -join ' ')"
    Write-Info "Test directories: $($testDirs -join ', ')"

    try {
        & python -m pytest @pytestArgs
        $exitCode = $LASTEXITCODE
    } catch {
        try {
            & python3 -m pytest @pytestArgs
            $exitCode = $LASTEXITCODE
        } catch {
            Write-Err "Failed to run pytest: $_"
            exit 1
        }
    }

    return $exitCode
}

# Main
function Main {
    Write-Host "PageSpeed IIS Integration Tests" -ForegroundColor Cyan
    Write-Host "================================" -ForegroundColor Cyan
    Write-Host ""

    $serverMode = if ($UseIISExpress) { "IIS Express" } else { "Full IIS" }
    Write-Info "Server mode: $serverMode"
    Write-Info "Target: http://${ServerHost}:${Port}/"

    # Check Python
    if (-not (Test-Python)) {
        Write-Err "Python is required but not found"
        exit 1
    }

    # Install dependencies
    if (-not $SkipInstall) {
        Install-Dependencies
    }

    # Start IIS Express if requested
    $startedIISExpress = $false
    if ($UseIISExpress) {
        Start-IISExpressServer
        $startedIISExpress = $true
    } elseif ($InstallSite) {
        # Install test site to full IIS if requested
        Install-TestSite
    }

    try {
        # Wait for IIS
        Wait-ForIIS

        # Run tests
        $exitCode = Run-Tests
    } finally {
        # Stop IIS Express if we started it
        if ($startedIISExpress) {
            Stop-IISExpressServer
        }
    }

    Write-Host ""
    if ($exitCode -eq 0) {
        Write-Info "All tests passed!"
    } else {
        Write-Err "Some tests failed (exit code: $exitCode)"
    }

    exit $exitCode
}

Main

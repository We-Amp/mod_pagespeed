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
    Run PageSpeed integration tests against IIS Express or Full IIS.

.DESCRIPTION
    This script builds the PageSpeed IIS module, sets up IIS (Express or Full),
    and runs the Python integration test suite. It mirrors the functionality
    of run_system_tests.sh for Apache and run_envoy_tests.sh for Envoy.

    By default it uses IIS Express, which works for interactive sessions.
    Use -UseFullIIS for CI/service contexts (Session 0) where IIS Express
    cannot run because it requires an interactive desktop session.

.PARAMETER TestFilter
    pytest filter expression (e.g., "sanity" or "test_extend_cache")

.PARAMETER BuildOnly
    Only build the module, don't run tests

.PARAMETER SkipBuild
    Skip building the module, just run tests

.PARAMETER KeepRunning
    Keep IIS running after tests complete

.PARAMETER Verbose
    Show verbose test output

.PARAMETER Port
    Port for IIS to listen on (default: 8080)

.PARAMETER UseFullIIS
    Use Full IIS (Windows Service) instead of IIS Express.
    Required when running from a Windows service (Session 0), e.g. GitHub Actions
    self-hosted runners installed as services. IIS Express cannot start in Session 0.

.EXAMPLE
    .\run_iis_tests.ps1
    .\run_iis_tests.ps1 -TestFilter "sanity"
    .\run_iis_tests.ps1 -BuildOnly
    .\run_iis_tests.ps1 -SkipBuild -KeepRunning
    .\run_iis_tests.ps1 -SkipBuild -UseFullIIS -Verbose   # CI mode
#>

param(
    [string]$TestFilter = "",
    [switch]$BuildOnly,
    [switch]$SkipBuild,
    [switch]$KeepRunning,
    [switch]$Verbose,
    [switch]$UseFullIIS,
    [int]$Port = 8080
)

$ErrorActionPreference = "Stop"

# Get script directory and source root
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceRoot = (Get-Item $ScriptDir).Parent.Parent.FullName

function Write-Banner {
    param([string]$Message)
    $line = "=" * 70
    Write-Host ""
    Write-Host $line -ForegroundColor Cyan
    Write-Host " $Message" -ForegroundColor Cyan
    Write-Host $line -ForegroundColor Cyan
    Write-Host ""
}

function Write-Status {
    param([string]$Message, [string]$Color = "White")
    Write-Host "[IIS Tests] " -NoNewline -ForegroundColor Cyan
    Write-Host $Message -ForegroundColor $Color
}

function Test-Prerequisites {
    Write-Status "Checking prerequisites..."

    # Check for Python
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        $python = Get-Command python3 -ErrorAction SilentlyContinue
    }
    if (-not $python) {
        Write-Status "Python not found! Please install Python 3.8+" "Red"
        exit 1
    }
    Write-Status "Python: $($python.Source)" "Gray"

    # Check for pip/pytest
    $pytest = & $python.Source -m pytest --version 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Status "pytest not found. Installing test dependencies..." "Yellow"
        & $python.Source -m pip install -r "$ScriptDir\requirements.txt"
    }
    Write-Status "pytest available" "Gray"

    # Check for Bazel (only if building)
    if (-not $SkipBuild) {
        $bazel = Get-Command bazel -ErrorAction SilentlyContinue
        if (-not $bazel) {
            $bazel = Get-Command bazelisk -ErrorAction SilentlyContinue
        }
        if (-not $bazel) {
            Write-Status "Bazel/Bazelisk not found! Please install Bazel." "Red"
            exit 1
        }
        Write-Status "Bazel: $($bazel.Source)" "Gray"
    }

    if ($UseFullIIS) {
        # Check for Full IIS (W3SVC service)
        $w3svc = Get-Service -Name W3SVC -ErrorAction SilentlyContinue
        if (-not $w3svc) {
            Write-Status "Full IIS (W3SVC) not found! Install the Web-Server Windows feature." "Red"
            exit 1
        }
        Write-Status "Full IIS: W3SVC service present (Status: $($w3svc.Status))" "Gray"
    } else {
        # Check for IIS Express
        $iisExpress = "${env:ProgramFiles}\IIS Express\iisexpress.exe"
        if (-not (Test-Path $iisExpress)) {
            $iisExpress = "${env:ProgramFiles(x86)}\IIS Express\iisexpress.exe"
        }
        if (-not (Test-Path $iisExpress)) {
            Write-Status "IIS Express not found!" "Red"
            Write-Status "Download from: https://www.microsoft.com/en-us/download/details.aspx?id=48264" "Yellow"
            exit 1
        }
        Write-Status "IIS Express: $iisExpress" "Gray"
    }
}

function Build-PageSpeedModule {
    Write-Banner "Building PageSpeed IIS Module"

    Push-Location $SourceRoot
    try {
        # Build the IIS module
        Write-Status "Building //pagespeed/iis:pagespeed_iis.dll..."

        $buildArgs = @(
            "build",
            "--config=windows",
            "--config=clang-cl",
            "//pagespeed/iis:pagespeed_iis.dll",
            "//pagespeed/kernel/license_v2:generate_license_token"
        )

        & bazel @buildArgs

        if ($LASTEXITCODE -ne 0) {
            Write-Status "Build failed!" "Red"
            exit 1
        }

        Write-Status "Build successful!" "Green"

        $dllPath = "$SourceRoot\bazel-bin\pagespeed\iis\pagespeed_iis.dll"
        Write-Status "Module ready at: $dllPath" "Gray"

        # Generate test license token and pre-seed the license file.
        # Write to both the default cache path parent (C:\PageSpeed) and the
        # configured fileCachePath parent (C:\) to cover both code paths.
        $tokenBin = "$SourceRoot\bazel-bin\pagespeed\kernel\license_v2\generate_license_token.exe"
        if (Test-Path $tokenBin) {
            $keyPath = if ($env:PAGESPEED_SIGNING_KEY) { $env:PAGESPEED_SIGNING_KEY } else { "$env:USERPROFILE\.weamp\license-signing-key" }
            if (-not (Test-Path $keyPath)) {
                Write-Warning "No signing key at $keyPath - set LICENSE_TOKEN env var instead"
            } else {
                $token = & $tokenBin --key $keyPath --sub "test@system-test.local" --exp-duration 3600
                @("C:\PageSpeed\pagespeed.license", "C:\pagespeed.license") | ForEach-Object {
                    Set-Content -Path $_ -Value $token -NoNewline
                    Write-Status "Test license written to $_" "Green"
                }
            }
        }
    } finally {
        Pop-Location
    }
}

function Start-TestServer {
    if ($UseFullIIS) {
        Write-Banner "Setting up Full IIS Test Server"
        & "$ScriptDir\setup_iis_full.ps1" -Action install -Port $Port -SourceRoot $SourceRoot
    } else {
        Write-Banner "Starting IIS Express Test Server"
        & "$ScriptDir\setup_iis_test.ps1" -Action start -Port $Port -SourceRoot $SourceRoot
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Status "Failed to start test server!" "Red"
        exit 1
    }
}

function Stop-TestServer {
    Write-Status "Stopping test server..."
    if ($UseFullIIS) {
        & "$ScriptDir\setup_iis_full.ps1" -Action stop -SourceRoot $SourceRoot
    } else {
        & "$ScriptDir\setup_iis_test.ps1" -Action stop -SourceRoot $SourceRoot
    }
}

function Run-Tests {
    Write-Banner "Running Integration Tests"

    # Set environment variables for tests
    $env:PAGESPEED_HOST = "localhost"
    $env:PAGESPEED_PORT = $Port
    $env:PAGESPEED_TEST_ROOT = "/mod_pagespeed_test"
    $env:PAGESPEED_EXAMPLE_ROOT = "/mod_pagespeed_example"
    $env:PAGESPEED_REWRITTEN_ROOT = "/mod_pagespeed_example"
    if ($UseFullIIS) {
        $env:PAGESPEED_CACHE_DIR = "C:\pagespeed_cache"
    } else {
        $env:PAGESPEED_CACHE_DIR = "$env:TEMP\pagespeed_iis_test\cache"
    }
    $env:PAGESPEED_STATS_ENABLED = "1"
    $env:PAGESPEED_SERVER_TYPE = "iis"
    $env:PAGESPEED_STATS_PATH = "/pagespeed_statistics"
    $env:PAGESPEED_ADMIN_PATH = "/pagespeed_admin"
    # Force unbuffered output so CI runners see pytest progress in real-time
    $env:PYTHONUNBUFFERED = "1"

    Write-Status "Test configuration:" "Cyan"
    Write-Status "  Host: $env:PAGESPEED_HOST" "Gray"
    Write-Status "  Port: $env:PAGESPEED_PORT" "Gray"
    Write-Status "  Test Root: $env:PAGESPEED_TEST_ROOT" "Gray"
    Write-Status "  Example Root: $env:PAGESPEED_EXAMPLE_ROOT" "Gray"
    Write-Status "  Cache Dir: $env:PAGESPEED_CACHE_DIR" "Gray"

    # Build pytest arguments (-u forces unbuffered stdout/stderr for CI)
    $pytestArgs = @(
        "-u",
        "-m", "pytest",
        "$ScriptDir\automatic",
        "-v",
        "--tb=short"
    )

    if ($TestFilter) {
        $pytestArgs += @("-k", $TestFilter)
    }

    if ($Verbose) {
        $pytestArgs += @("-s", "--capture=no")
    }

    # Add markers to skip tests that require features not yet implemented
    # These can be removed as features are added to the IIS module
    $pytestArgs += @(
        "--deselect=automatic/test_https.py",  # HTTPS not configured yet
        "-m", "not requires_secondary and not requires_https"
    )

    Write-Status "Running: python $($pytestArgs -join ' ')" "Gray"
    Write-Host ""

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        $python = Get-Command python3 -ErrorAction SilentlyContinue
    }

    & $python.Source @pytestArgs | Out-Host
    $testResult = $LASTEXITCODE

    return $testResult
}

# Main execution
try {
    if ($UseFullIIS) {
        Write-Banner "PageSpeed IIS Integration Tests (Full IIS)"
    } else {
        Write-Banner "PageSpeed IIS Integration Tests (IIS Express)"
    }

    Test-Prerequisites

    if (-not $SkipBuild) {
        Build-PageSpeedModule

        if ($BuildOnly) {
            Write-Status "Build complete. Exiting (--BuildOnly specified)." "Green"
            exit 0
        }
    }

    Start-TestServer

    $testResult = Run-Tests

    if ($testResult -eq 0) {
        Write-Banner "All Tests Passed!"
        Write-Status "SUCCESS" "Green"
    } else {
        Write-Banner "Some Tests Failed"
        Write-Status "FAILURE (exit code: $testResult)" "Red"
    }

    exit $testResult
} finally {
    if (-not $KeepRunning) {
        Stop-TestServer
    } else {
        Write-Status "Keeping server running (--KeepRunning specified)" "Yellow"
        if ($UseFullIIS) {
            Write-Status "Stop with: .\setup_iis_full.ps1 stop" "Yellow"
        } else {
            Write-Status "Stop with: .\setup_iis_test.ps1 stop" "Yellow"
        }
    }
}

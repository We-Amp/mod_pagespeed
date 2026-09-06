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
            "//pagespeed/iis:pagespeed_iis.dll"
        )

        & bazel @buildArgs

        if ($LASTEXITCODE -ne 0) {
            Write-Status "Build failed!" "Red"
            exit 1
        }

        Write-Status "Build successful!" "Green"

        $dllPath = "$SourceRoot\bazel-bin\pagespeed\iis\pagespeed_iis.dll"
        Write-Status "Module ready at: $dllPath" "Gray"
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

function Show-FailureEvidence {
    # On failure, snapshot the module's in-process diagnostics BEFORE teardown
    # recycles the worker: statistics and the message buffer live in w3wp's
    # memory and are unrecoverable once Stop-TestServer runs. The fetch counters
    # go to the console unconditionally -- they distinguish a real fetch failure
    # feeding the 5-minute negative cache (recent_fetch_failure climbing) from a
    # rewrite that merely ran out of fetch_until budget (flat counters). Full
    # pages are written only when PAGESPEED_EVIDENCE_DIR is set (CI points it at
    # the directory its evidence artifact uploads).
    $base = "http://localhost:$Port"
    $evidenceDir = $env:PAGESPEED_EVIDENCE_DIR

    try {
        $resp = Invoke-WebRequest -Uri "$base/pagespeed_statistics" -UseBasicParsing -TimeoutSec 15
        if ($evidenceDir) {
            New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
            Set-Content -Path (Join-Path $evidenceDir "stats-on-failure.txt") -Value $resp.Content
        }
        $vars = ($resp.Content | ConvertFrom-Json).variables
        Write-Status "Fetch-path counters at failure:" "Yellow"
        $vars.PSObject.Properties |
            Where-Object { $_.Name -match 'fetch|remember' } |
            ForEach-Object { Write-Status ("  {0} = {1}" -f $_.Name, $_.Value) "Yellow" }
    } catch {
        Write-Status "Could not capture /pagespeed_statistics: $($_.Exception.Message)" "Yellow"
    }

    if ($evidenceDir) {
        try {
            # NOT /pagespeed_message: the module routes that path but has no
            # handler for it (falls through to the static handler, 404). The
            # AdminSite page is the one that serves the in-memory buffer.
            $resp = Invoke-WebRequest -Uri "$base/pagespeed_admin/message_history" -UseBasicParsing -TimeoutSec 15
            Set-Content -Path (Join-Path $evidenceDir "messages-on-failure.txt") -Value $resp.Content
        } catch {
            Write-Status "Could not capture message_history: $($_.Exception.Message)" "Yellow"
        }
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
    # HTTPS coverage is wired into the Full IIS harness only
    # (setup_iis_full.ps1 adds an https binding + self-signed cert on 8443).
    # IIS Express (setup_iis_test.ps1) stays HTTP-only, so leave HTTPS
    # unconfigured there -- the HTTPS tests self-skip when PAGESPEED_HTTPS_HOST
    # is unset (module-level skipif).
    if ($UseFullIIS) {
        $env:PAGESPEED_HTTPS_HOST = "localhost"
        $env:PAGESPEED_HTTPS_PORT = "8443"
    }
    # Force unbuffered output so CI runners see pytest progress in real-time
    $env:PYTHONUNBUFFERED = "1"

    Write-Status "Test configuration:" "Cyan"
    Write-Status "  Host: $env:PAGESPEED_HOST" "Gray"
    Write-Status "  Port: $env:PAGESPEED_PORT" "Gray"
    Write-Status "  Test Root: $env:PAGESPEED_TEST_ROOT" "Gray"
    Write-Status "  Example Root: $env:PAGESPEED_EXAMPLE_ROOT" "Gray"
    Write-Status "  Cache Dir: $env:PAGESPEED_CACHE_DIR" "Gray"

    # Build pytest arguments (-u forces unbuffered stdout/stderr for CI).
    # Collect from both automatic/ (cross-platform tests) and iis/ (IIS-specific
    # tests gated by @pytest.mark.iis_only -- skipped automatically when
    # PAGESPEED_SERVER_TYPE != "iis" via conftest.py's pytest_runtest_setup).
    $pytestArgs = @(
        "-u",
        "-m", "pytest",
        "$ScriptDir\automatic",
        "$ScriptDir\iis",
        "-v",
        "--tb=short"
    )

    if ($TestFilter) {
        $pytestArgs += @("-k", $TestFilter)
    }

    if ($Verbose) {
        $pytestArgs += @("-s", "--capture=no")
    }

    # Skip secondary-cache tests: the IIS harness is single-site with no
    # secondary vhost.
    #
    # IIS HTTPS coverage: iis/test_iis_https.py (listener + resource rewriting
    # over TLS) plus the cross-platform automatic/test_https.py, both enabled by
    # PAGESPEED_HTTPS_HOST under -UseFullIIS. They self-skip when HTTPS is
    # unconfigured (e.g. IIS Express) via a module-level skipif. Sub-resource
    # rewriting over TLS works via the WINHTTP_FLAG_SECURE fix.
    $pytestArgs += @(
        "-m", "not requires_secondary"
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
        Show-FailureEvidence
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

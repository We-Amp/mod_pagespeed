# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
    Starts IIS Express with PageSpeed configuration.

.DESCRIPTION
    Starts IIS Express using the configuration created by Setup-IISExpress.ps1.
    Can run in foreground or background mode.

.PARAMETER Background
    Run IIS Express in the background.

.PARAMETER Port
    Port to use. Default: 8080

.PARAMETER Wait
    Wait for IIS Express to be ready before returning.

.EXAMPLE
    .\Start-IISExpress.ps1

.EXAMPLE
    .\Start-IISExpress.ps1 -Background
#>

param(
    [switch]$Background,
    [int]$Port = 8080,
    [switch]$Wait
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ConfigPath = Join-Path $env:TEMP "pagespeed_iisexpress"
$ConfigFile = Join-Path $ConfigPath "applicationhost.config"

# Run setup if config doesn't exist
if (-not (Test-Path $ConfigFile)) {
    Write-Host "Running setup..." -ForegroundColor Yellow
    & "$ScriptDir\Setup-IISExpress.ps1" -Port $Port
}

# Find IIS Express
$IISExpressPath = ""
$PossiblePaths = @(
    "${env:ProgramFiles}\IIS Express\iisexpress.exe",
    "${env:ProgramFiles(x86)}\IIS Express\iisexpress.exe"
)

foreach ($path in $PossiblePaths) {
    if (Test-Path $path) {
        $IISExpressPath = $path
        break
    }
}

if (-not $IISExpressPath) {
    Write-Error "IIS Express not found"
    exit 1
}

# Check if already running
$existing = Get-Process -Name "iisexpress" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match "PageSpeedTest" }

if ($existing) {
    Write-Host "IIS Express is already running (PID: $($existing.Id))" -ForegroundColor Yellow
    exit 0
}

# Set document root for localhost resource fetches.
# The PageSpeed module's CurlUrlAsyncFetcher reads this to serve localhost
# resources directly from disk, avoiding HTTP deadlock.
$SitePath = Join-Path $ScriptDir "testsite"
$env:PAGESPEED_DOCUMENT_ROOT = $SitePath
Write-Host "PAGESPEED_DOCUMENT_ROOT=$SitePath" -ForegroundColor Gray

# Start IIS Express
Write-Host "Starting IIS Express on port $Port..." -ForegroundColor Green

$args = @("/config:$ConfigFile", "/site:PageSpeedTest")

if ($Background) {
    $process = Start-Process -FilePath $IISExpressPath -ArgumentList $args `
        -WindowStyle Hidden -PassThru
    Write-Host "IIS Express started in background (PID: $($process.Id))" -ForegroundColor Green

    # Save PID for Stop-IISExpress.ps1
    $process.Id | Out-File -FilePath (Join-Path $ConfigPath "iisexpress.pid")

    if ($Wait) {
        Write-Host "Waiting for IIS Express to be ready..." -ForegroundColor Yellow
        $ready = $false
        $attempts = 0
        $maxAttempts = 30

        while (-not $ready -and $attempts -lt $maxAttempts) {
            $attempts++
            try {
                $response = Invoke-WebRequest -Uri "http://localhost:$Port/" `
                    -UseBasicParsing -TimeoutSec 2 -ErrorAction SilentlyContinue
                if ($response.StatusCode -eq 200) {
                    $ready = $true
                }
            } catch {
                Start-Sleep -Seconds 1
            }
        }

        if ($ready) {
            Write-Host "IIS Express is ready!" -ForegroundColor Green
        } else {
            Write-Warning "IIS Express may not be fully started after $maxAttempts seconds"
        }
    }
} else {
    # Run in foreground
    Write-Host "Running IIS Express in foreground (Ctrl+C to stop)..." -ForegroundColor Yellow
    & $IISExpressPath $args
}

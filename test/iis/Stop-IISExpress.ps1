<#
.SYNOPSIS
    Stops IIS Express.

.DESCRIPTION
    Stops the IIS Express instance started by Start-IISExpress.ps1.

.EXAMPLE
    .\Stop-IISExpress.ps1
#>

$ErrorActionPreference = "SilentlyContinue"
$ConfigPath = Join-Path $env:TEMP "pagespeed_iisexpress"
$PidFile = Join-Path $ConfigPath "iisexpress.pid"

# Try to stop by PID file
if (Test-Path $PidFile) {
    $pid = Get-Content $PidFile
    $process = Get-Process -Id $pid -ErrorAction SilentlyContinue
    if ($process) {
        Write-Host "Stopping IIS Express (PID: $pid)..." -ForegroundColor Yellow
        Stop-Process -Id $pid -Force
        Write-Host "IIS Express stopped" -ForegroundColor Green
    }
    Remove-Item $PidFile -Force
}

# Also try to stop any IIS Express running PageSpeedTest
$processes = Get-Process -Name "iisexpress" -ErrorAction SilentlyContinue
foreach ($proc in $processes) {
    try {
        $cmdLine = (Get-WmiObject Win32_Process -Filter "ProcessId = $($proc.Id)").CommandLine
        if ($cmdLine -match "PageSpeedTest") {
            Write-Host "Stopping IIS Express (PID: $($proc.Id))..." -ForegroundColor Yellow
            Stop-Process -Id $proc.Id -Force
            Write-Host "IIS Express stopped" -ForegroundColor Green
        }
    } catch {
        # Ignore errors
    }
}

Write-Host "Done" -ForegroundColor Green

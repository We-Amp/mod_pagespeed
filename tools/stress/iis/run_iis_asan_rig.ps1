# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

<#
.SYNOPSIS
  Drive tools/stress/stress_shutdown.py against the ASan IIS rig with periodic
  app-pool recycle / stop-start / cache-flush chaos.

.DESCRIPTION
  The recycle (appcmd recycle apppool) and stop/start cycles exercise
  DLL_PROCESS_DETACH and worker-vs-static teardown under live load -- the exact
  window where the shutdown-race (spdlog UAF) and dll_main.cc memory bugs live
  and which the win-asan unit-test job structurally cannot reach.

  Exit code is the harness's: non-zero on any ASan/crash hit or post-restart
  non-200. Evidence lands in <RigDir>\logs.
#>
param(
  [string]$RigDir      = $(if (Test-Path 'D:\') { 'D:\iis-asan-rig' } else { 'C:\iis-asan-rig' }),
  [string]$PoolName    = 'PageSpeedAsanStressPool',
  [string]$SiteName    = 'PageSpeedAsanStressSite',
  [int]$Port           = 18180,
  [int]$Duration       = 600,
  [int]$Concurrency    = 16,
  [string]$CoredumpDir = 'C:\CrashDumps',
  [string]$RepoDir     = $(Get-Location)
)
$ErrorActionPreference = 'Continue'
$ac = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
# Prefer explicit python installs; `Get-Command python` first hits the WindowsApps
# App-Execution-Alias stub, which fails to run.
$py = @("C:\Program Files\Python312\python.exe","$env:LOCALAPPDATA\Programs\Python\Python312\python.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $py) { $py = (Get-Command python -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1).Source }
if (-not $py) { Write-Error "No usable python found (WindowsApps stub excluded)"; exit 1 }

New-Item -ItemType Directory -Path $CoredumpDir -Force | Out-Null

# reload = graceful recycle (new worker, old drains); restart = hard stop+start of pool+site.
# ping -n 3 is a ~2s sleep with no external deps (matches the proven D:\stress349 recipe).
$reload  = "$ac recycle apppool /apppool.name:$PoolName"
$restart = "$ac stop apppool /apppool.name:$PoolName & $ac stop site /site.name:$SiteName & ping -n 3 127.0.0.1 >nul & $ac start apppool /apppool.name:$PoolName & $ac start site /site.name:$SiteName"

"started $(Get-Date -Format o)" | Set-Content "$RigDir\logs\harness.started"
Set-Location $RepoDir
& $py "$RepoDir\tools\stress\stress_shutdown.py" run --server iis `
  --base-url "http://localhost:$Port" --url-prefix /stress `
  --cache-dir "$RigDir\cache" --duration $Duration --concurrency $Concurrency `
  --reload-cmd $reload --restart-cmd $restart `
  --error-log "$RigDir\logs\error.log" --coredump-dir $CoredumpDir `
  *> "$RigDir\logs\harness.log"
$rc = $LASTEXITCODE
"exit=$rc" | Set-Content "$RigDir\logs\harness.exit"
"done $(Get-Date -Format o)" | Add-Content "$RigDir\logs\harness.exit"
Write-Host "harness exit=$rc (log: $RigDir\logs\harness.log)"
if ($rc -eq 0) {
  # The harness only sees HTTP responses and its own tailed logs -- it once
  # printed PASS over 8 w3wp faults (the dying pools surfaced as 503s,
  # which it does not treat as crashes). The event-log/dump sweep is the verdict.
  Write-Host "NOTE: harness rc=0 is NOT the verdict -- sweep_iis_asan_rig.ps1 adjudicates"
}
exit $rc

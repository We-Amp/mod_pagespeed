<#
.SYNOPSIS
  Tear down the ASan IIS rig and restore the CI PageSpeedModule.

.DESCRIPTION
  MUST run even on failure (CI: if: always()). Deletes the rig site/pool and the
  ASan module, then restores the CI PageSpeedModule from the image path snapshotted
  by setup (-RigDir\ci-module-image.txt). A missed restore self-heals on the next
  IIS-systest run (setup_iis_full.ps1 reinstalls PageSpeedModule from scratch), but
  we restore for hygiene and fail LOUD if we cannot, so the runner is never left in
  a silently-degraded state.
#>
param(
  [string]$RigDir     = $(if (Test-Path 'D:\') { 'D:\iis-asan-rig' } else { 'C:\iis-asan-rig' }),
  [string]$PoolName   = 'PageSpeedAsanStressPool',
  [string]$SiteName   = 'PageSpeedAsanStressSite',
  [string]$ModuleName = 'PageSpeedAsanModule'
)
$ErrorActionPreference = 'Continue'
$ac = "$env:SystemRoot\System32\inetsrv\appcmd.exe"

# 0. Remove the WER LocalDumps override installed by setup. Leaving
# it in place would silently full-dump every future w3wp crash on the runner.
$werKey = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\w3wp.exe'
if (Test-Path $werKey) {
  Remove-Item $werKey -Force -ErrorAction SilentlyContinue
  Write-Host "WER LocalDumps\w3wp.exe removed: $(if (Test-Path $werKey) { 'FAILED' } else { 'ok' })"
}

# 1. Stop site + pool, let the worker drain
& $ac stop site $SiteName    2>$null
& $ac stop apppool $PoolName 2>$null
Start-Sleep -Seconds 3
Get-Process w3wp -ErrorAction SilentlyContinue | ForEach-Object { $_ | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue }

# 2. Remove the site-level module enablement (location tag in applicationHost.config)
& $ac set config "$SiteName/" -section:system.webServer/modules "/-[name='$ModuleName']" /commit:apphost 2>$null
Write-Host "site-level ASan module removal exit: $LASTEXITCODE"

# 3. Delete site + pool
& $ac delete site $SiteName    2>$null
& $ac delete apppool $PoolName 2>$null

# 4. Deregister the ASan global module (belt-and-suspenders on the globalModules entry)
& $ac uninstall module $ModuleName 2>$null
Write-Host "uninstall ASan module exit: $LASTEXITCODE"
$gm = (& $ac list config -section:system.webServer/globalModules) | Select-String "name=`"$ModuleName`""
if ($gm) {
  & $ac set config -section:system.webServer/globalModules "/-[name='$ModuleName']" /commit:apphost 2>$null
}

# 5. Restore the CI PageSpeedModule from the snapshotted image path
$snap = "$RigDir\ci-module-image.txt"
if (Test-Path $snap) {
  $ciImage = (Get-Content $snap -Raw).Trim()
  if ($ciImage) {
    & $ac install module /name:PageSpeedModule /image:"$ciImage" /add:true 2>$null
    Write-Host "restore CI PageSpeedModule (/image:$ciImage) exit: $LASTEXITCODE"
    $ok = (& $ac list config -section:system.webServer/globalModules) | Select-String "name=`"PageSpeedModule`""
    if (-not $ok) {
      Write-Error "FAILED to restore CI PageSpeedModule from $ciImage -- runner needs manual check (next iis-sys-tests run reinstalls it, but verify)."
      exit 1
    }
  }
} else {
  Write-Host "No CI-module snapshot found -- nothing was removed, or the next IIS-systest run will reinstall PageSpeedModule."
}

# 6. Final verification
Write-Host "==== final state ===="
Write-Host "--- globalModules PageSpeed entries ---"
(& $ac list config -section:system.webServer/globalModules) | Select-String "PageSpeed" | ForEach-Object { $_.Line.Trim() }
Write-Host "CLEANUP DONE"

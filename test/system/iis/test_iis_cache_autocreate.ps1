# test_iis_cache_autocreate.ps1 - Pins the cache auto-initialization contract
# for the IIS module under the CYCLONE cache (mod_pagespeed 1.15 / v1.15.0+).
#
# Live-VM verified 2026-06-22 on the Windows Server 2016 IIS base image / v1.15.0:
# the original fixture asserted the OLD per-site FILE-cache layout
# (<FileCachePath>\<siteid>.ROOT subdirectories + per-subdir ACLs). v1.15.0 uses
# the CYCLONE cache - a single `cyclone.dat` store under FileCachePath, with NO
# per-site subdirectories and no per-subdir ACL dance. The probe showed the
# cache root contains only `cyclone.dat`; a per-site `<id>.ROOT` subdir is never
# created. The old assertions are therefore obsolete. This fixture now pins the
# REACHABLE Cyclone contract:
#
#   1. Positive: with FileCachePath at its shipped value and the cache contents
#      cleared (the root kept - the module auto-creates the store UNDER the root,
#      not the root itself; a deleted root surfaces cache-path-missing), the
#      module re-initializes its Cyclone store (a FRESH `cyclone.dat`, after the
#      setup asserts the old one is gone) under FileCachePath on request, serves
#      OPTIMIZED responses (X-Page-Speed present), and emits no
#      X-Pagespeed-Init-Status. NOTE: this pins store-file auto-init + an
#      optimizing response; it does not distinguish a live Cyclone backend from a
#      degraded LRU-only fallback that still writes a .dat (system_cache_path.cc
#      "Falling back to LRU-only cache" keeps serving X-Page-Speed). TODO: add a
#      live-Cyclone health signal once the out-of-tree lib exposes one.
#   2. Negative (cache-path-missing): with FileCachePath pointed at an explicit
#      OUT-OF-auto-create-prefix, non-existent directory, the module does not
#      auto-create it (iis_process_context.cpp IsPathInAutoCreatePrefix=false ->
#      legacy GetFileAttributes branch) and surfaces X-Pagespeed-Init-Status:
#      cache-path-missing (iis_http_module.cpp ~:501, InitFailureKind::kCachePathMissing).
#
# (The old cache-path-create-failed / per-site-ACL negative paths are dropped:
# they were file-cache-specific and are not part of the Cyclone store contract.)
#
# Config is resolved at runtime via FindConfigFile(<site app physical path>) =
# <site root>\pagespeed.config (the file the module reads at request time), NOT
# the ProgramData copy the old default edited. Cache state is cleared
# in setup, not teardown, with pre-state asserted.
#
# Pre-conditions (asserted; aborts cleanly if absent):
#   - PageSpeed IIS module installed + registered + LICENSED
#   - Default IIS site responding on http://localhost/
#   - Healthy state: X-Page-Speed present, no X-Pagespeed-Init-Status
#
# Run as Administrator (appcmd recycle + Remove-Item on ProgramData require it).

[CmdletBinding()]
param(
    [string]$Url = "http://localhost/",
    # Empty -> auto-resolve the config the module reads at request time via
    # FindConfigFile(<app physical path of $SiteName>).
    [string]$ConfigPath = "",
    [string]$SiteName = "Default Web Site",
    [string]$AppPool = "DefaultAppPool",
    [string]$CacheRoot = "C:\ProgramData\We-Amp\PageSpeed\cache",
    # Bogus FileCachePath for the negative path: explicit, OUT-OF-prefix,
    # non-existent -> the module leaves it missing and surfaces cache-path-missing.
    [string]$OutOfPrefixPath = "C:\nonexistent-pagespeed-autocreate-182\cache",
    [int]$PollSeconds = 30
)

$ErrorActionPreference = 'Stop'
$appcmd = "$env:SystemRoot\system32\inetsrv\appcmd.exe"

# Resolve the config the IIS module reads at request time, mirroring
# pagespeed/iis/iis_configuration.h FindConfigFile(): <appPhysicalPath>\
# pagespeed.config else <appPhysicalPath>\iiswebspeed.config (expand env vars).
function Resolve-ModuleConfigPath {
    param([string]$site)
    $raw = (& $appcmd list vdir "$site/" /text:physicalPath) 2>$null
    if (-not $raw) { return $null }
    $dir = [System.Environment]::ExpandEnvironmentVariables($raw.Trim())
    $primary = Join-Path $dir "pagespeed.config"
    if (Test-Path -LiteralPath $primary) { return $primary }
    $fallback = Join-Path $dir "iiswebspeed.config"
    if (Test-Path -LiteralPath $fallback) { return $fallback }
    return $primary
}

function Get-PsResponse {
    param([string]$u)
    try { return Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 5 } catch { return $null }
}
function Get-PsHeaders {
    param([string]$u)
    $r = Get-PsResponse -u $u
    if ($r) { return $r.Headers } else { return $null }
}
function Wait-ForHeader {
    param([string]$u, [string]$name, [string]$expectedValue = $null, [bool]$expectPresent = $true, [int]$timeoutSec = 30)
    $elapsed = 0
    while ($elapsed -lt $timeoutSec) {
        $h = Get-PsHeaders -u $u
        if ($h) {
            $v = $h[$name]
            if ($expectPresent) { if ($v) { if (-not $expectedValue -or $v -eq $expectedValue) { return $v } } }
            else { if (-not $v) { return $null } }
        }
        Start-Sleep -Seconds 2; $elapsed += 2
    }
    return [string]::Empty
}
function Wait-ForFile {
    param([string]$path, [int]$timeoutSec = 30)
    $elapsed = 0
    while ($elapsed -lt $timeoutSec) {
        if (Test-Path -LiteralPath $path) { return $true }
        # Keep traffic flowing so the Cyclone store is written.
        Get-PsResponse -u $script:Url | Out-Null
        Start-Sleep -Seconds 2; $elapsed += 2
    }
    return $false
}
function Recycle-AppPool {
    param([string]$pool)
    & $appcmd recycle apppool /apppool.name:$pool | Out-Null
    Start-Sleep -Seconds 3
}
function Stop-W3SVC { Stop-Service -Name W3SVC -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
function Start-W3SVC { Start-Service -Name W3SVC -ErrorAction SilentlyContinue; Start-Sleep -Seconds 3 }

# Clear cache CONTENTS but keep the root directory: the Cyclone store is created
# UNDER the root; a deleted root is not auto-created (surfaces cache-path-missing).
function Clear-CacheContents {
    param([string]$root)
    Stop-W3SVC
    $dat = Join-Path $root "cyclone.dat"
    # Retry the clear: w3wp may not have released the cyclone.dat handle the
    # instant W3SVC stops. Retry the delete a few times before giving up.
    for ($i = 0; $i -lt 5; $i++) {
        if (Test-Path -LiteralPath $root) {
            Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue |
                Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path -LiteralPath $dat)) { break }
        Start-Sleep -Seconds 2
    }
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    # Prove the Cyclone store is actually GONE. A surviving cyclone.dat would let
    # the positive path FALSE-PASS on a stale file instead of a genuine fresh
    # auto-init -- the very contract this gating fixture exists to assert.
    if (Test-Path -LiteralPath $dat) {
        throw "Setup: cyclone.dat still present after clear + retries (w3wp handle not released?): $dat"
    }
    Start-W3SVC
}

# --- Resolve config ---
if (-not $ConfigPath) {
    $ConfigPath = Resolve-ModuleConfigPath -site $SiteName
    Write-Host "Resolved module config (FindConfigFile of '$SiteName'): $ConfigPath"
}

# --- Pre-conditions (run BEFORE any state mutation) ---
Write-Host "=== Pre-conditions ==="
if (-not $ConfigPath -or -not (Test-Path -LiteralPath $ConfigPath)) {
    Write-Error "pagespeed.config not found at '$ConfigPath' (resolved from site '$SiteName'; is the module installed?)"
    exit 2
}
$baseline = Get-PsHeaders -u $Url
if (-not $baseline) { Write-Error "Cannot reach $Url - IIS not responding."; exit 2 }

$cycloneDat = Join-Path $CacheRoot "cyclone.dat"

# --- Backup config + define restore BEFORE any state mutation. The script-scope
#     trap (active from parse time) must only ever RESTORE the captured config,
#     never blank it -- so capture $originalContent first, and guard Restore-Config
#     against a $null capture. ---
$restored = $false
$originalContent = $null
$backupPath = "$ConfigPath.autocreate-backup"
$originalContent = Get-Content -Raw -LiteralPath $ConfigPath
Copy-Item -LiteralPath $ConfigPath -Destination $backupPath -Force
function Restore-Config {
    if ($script:restored) { return }
    # If the trap fires before the backup was captured, do NOT write $null over
    # the live config (the file the module reads at request time).
    if ($null -eq $script:originalContent) { return }
    Write-Host "Restoring original pagespeed.config..."
    Set-Content -LiteralPath $ConfigPath -Value $script:originalContent -NoNewline
    Recycle-AppPool -pool $AppPool
    Start-W3SVC   # leave W3SVC up on every exit path (negative path stops it)
    $script:restored = $true
}
trap { Restore-Config; break }

try {
    # --- Setup: clear cache state in setup, not teardown. INSIDE
    #     the try so a setup-time throw both runs the finally (backup-file
    #     cleanup) and lets the trap restore (never blank) the config. ---
    Write-Host "=== Setup: clear cache contents ==="
    Clear-CacheContents -root $CacheRoot
    Write-Host "Pre-state OK: cache root present, cyclone.dat cleared ($CacheRoot)."

    # ============================================================
    # POSITIVE PATH: Cyclone store auto-initializes under FileCachePath.
    # ============================================================
    Write-Host ""
    Write-Host "=== Positive path: Cyclone store auto-init on default config ==="
    Clear-CacheContents -root $CacheRoot
    Recycle-AppPool -pool $AppPool

    $response = $null; $elapsed = 0
    while ($elapsed -lt $PollSeconds -and -not $response) {
        $response = Get-PsResponse -u $Url
        if (-not $response) { Start-Sleep -Seconds 2; $elapsed += 2 }
    }
    if (-not $response) { throw "Positive path: no response from $Url within ${PollSeconds}s." }
    if ($response.StatusCode -ne 200) { throw "Positive path: expected 200, got $($response.StatusCode)." }
    Write-Host "Got HTTP $($response.StatusCode) from $Url."

    # The very first request after a recycle races init: poll for the
    # diagnostic header to be ABSENT (mirrors the diagnostic fixture idiom).
    $cleared = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" -expectPresent $false -timeoutSec $PollSeconds
    if ($cleared -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw "Positive path: X-Pagespeed-Init-Status did not clear within ${PollSeconds}s; observed '$obs'."
    }

    # Assert: the Cyclone store (cyclone.dat) was auto-created UNDER FileCachePath.
    if (-not (Wait-ForFile -path $cycloneDat -timeoutSec $PollSeconds)) {
        Write-Host "cache root contents:"; Get-ChildItem -LiteralPath $CacheRoot -Force -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $($_.Name)" }
        throw "Positive path: Cyclone store '$cycloneDat' was not created within ${PollSeconds}s."
    }
    Write-Host "Cyclone store auto-created: $cycloneDat"

    # Assert: module is healthy + licensed (optimizing -> X-Page-Speed present).
    $xps = Wait-ForHeader -u $Url -name "X-Page-Speed" -timeoutSec $PollSeconds
    if (-not $xps) { throw "Positive path: X-Page-Speed header absent (module not optimizing/licensed)." }
    Write-Host "PASS: positive path (cyclone.dat created, X-Page-Speed=$xps, no init-status)."

    # ============================================================
    # NEGATIVE PATH: cache-path-missing via an out-of-prefix, missing path.
    # ============================================================
    Write-Host ""
    Write-Host "=== Negative path: cache-path-missing (out-of-prefix FileCachePath) ==="
    Stop-W3SVC
    if (Test-Path -LiteralPath $OutOfPrefixPath) { Remove-Item -LiteralPath $OutOfPrefixPath -Recurse -Force }
    $patched = $originalContent -replace `
        '(?m)^\s*(?:ModPagespeed|pagespeed\s+)FileCachePath\s+.*$', `
        ('pagespeed FileCachePath "' + $OutOfPrefixPath + '"')
    if ($patched -eq $originalContent) {
        $patched = $originalContent.TrimEnd() + "`npagespeed FileCachePath `"$OutOfPrefixPath`"`n"
    }
    Set-Content -LiteralPath $ConfigPath -Value $patched -NoNewline
    Start-W3SVC
    Recycle-AppPool -pool $AppPool

    $diag = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" -expectedValue "cache-path-missing" -timeoutSec $PollSeconds
    if ($diag -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw "Negative path: expected X-Pagespeed-Init-Status='cache-path-missing' within ${PollSeconds}s; observed '$obs'."
    }
    Write-Host "PASS: negative path (header='$diag')."

    Restore-Config

    # ============================================================
    # Post-restore sanity: healthy state returns.
    # ============================================================
    Write-Host ""
    Write-Host "=== Asserting healthy state restored ==="
    $cleared2 = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" -expectPresent $false -timeoutSec $PollSeconds
    if ($cleared2 -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw "Post-restore: X-Pagespeed-Init-Status did not clear within ${PollSeconds}s; observed '$obs'."
    }
    $xps2 = Wait-ForHeader -u $Url -name "X-Page-Speed" -timeoutSec $PollSeconds
    if (-not $xps2) { throw "Post-restore: X-Page-Speed did not return within ${PollSeconds}s." }
    Write-Host "PASS: healthy state restored (X-Page-Speed=$xps2)."

    Write-Host ""
    Write-Host "=== test_iis_cache_autocreate.ps1: ALL PASS ==="
    exit 0
}
finally {
    Restore-Config
    if (Test-Path -LiteralPath $backupPath) {
        Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $OutOfPrefixPath) {
        Remove-Item -LiteralPath $OutOfPrefixPath -Recurse -Force -ErrorAction SilentlyContinue
    }
}

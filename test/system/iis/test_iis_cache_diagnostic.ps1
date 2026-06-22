# test_iis_cache_diagnostic.ps1 - Regression guard for the
# X-Pagespeed-Init-Status response header emitted by
# pagespeed/iis/iis_process_context.cpp + iis_http_module.cpp when the
# module fails to initialize because the configured FileCachePath does not
# resolve to a usable directory.
#
# The header (and the matching per-failure-mode local-only error page) is
# the diagnostic mechanism that justifies Return="ignore" on the
# GrantCacheAcl / GrantLogAcl deferred-exe CAs in install/iis/Product.wxs:
# if those CAs silently fail to grant IIS_IUSRS write on the cache dir, the
# module surfaces the failure to localhost admin on the very first request
# instead of failing the MSI mid-install. The whole pattern is worthless if
# a future regression silently drops the header, so this test pins the
# header-emission contract.
#
# Live-VM verified on the Windows Server 2016 IIS base image, 2026-06-22 - two earlier
# bugs in this fixture are fixed here:
#
#   1. WRONG CONFIG FILE. At request time the module resolves its config via
#      FindConfigFile(GetApplicationPhysicalPath()) =
#      <site-physical-path>\pagespeed.config (else \iiswebspeed.config) -
#      see pagespeed/iis/iis_configuration.h + iis_http_module.cpp:449. That
#      is the SITE root (e.g. C:\inetpub\wwwroot\pagespeed.config), NOT
#      C:\ProgramData\We-Amp\PageSpeed\pagespeed.config (the MSI installs a
#      copy to BOTH). Editing the ProgramData copy - the old fixture default
#      - never affected what the module reads, so the failure mode never
#      triggered. This fixture now resolves + edits the SITE config the
#      module actually reads, the same way the module resolves it.
#
#   2. UNREACHABLE FAILURE MODE. On a shipped install, NONE of `FileCachePath
#      ""`, deleting the directive, or emptying the whole config file
#      produces kCachePathEmpty (the module falls back to a working default
#      cache path -> inits fine -> no header; all three verified on the VM).
#      The reliably-reachable diagnostic via a config edit is
#      kCachePathMissing: point FileCachePath at an explicit,
#      out-of-auto-create-prefix, non-existent directory. The module does not
#      auto-create out-of-prefix paths (iis_process_context.cpp:324
#      IsPathInAutoCreatePrefix), so it leaves the path missing and surfaces
#      X-Pagespeed-Init-Status: cache-path-missing (iis_http_module.cpp:503).
#      This fixture asserts that.
#
# Provisions the failure mode, recycles the IIS app pool to force module
# re-init, asserts the header, then restores the original config and confirms
# the healthy state returns.
#
# Pre-conditions (asserted; aborts cleanly if absent):
#   - PageSpeed IIS module already installed + registered + LICENSED
#   - Default IIS site responding on http://localhost/
#   - Healthy state: X-Page-Speed header present, no X-Pagespeed-Init-Status
#
# Run as Administrator (appcmd recycle requires it).

[CmdletBinding()]
param(
    [string]$Url = "http://localhost/",
    # Empty -> auto-resolve the config the module actually reads, the same way
    # the module does: FindConfigFile(<app physical path of $SiteName>).
    # Override only to point at a specific file (e.g. a legacy-layout install).
    [string]$ConfigPath = "",
    [string]$SiteName = "Default Web Site",
    [string]$AppPool = "DefaultAppPool",
    [int]$PollSeconds = 30
)

$ErrorActionPreference = 'Stop'
$appcmd = "$env:SystemRoot\system32\inetsrv\appcmd.exe"

# A deliberately bogus FileCachePath: an explicit, OUT-OF-auto-create-prefix,
# non-existent directory. The module only auto-creates paths under
# ...\We-Amp\PageSpeed\cache\ or ...\We-Amp\IISWebSpeed\cache\, so this path is
# left missing and surfaces kCachePathMissing rather than being created.
$BogusCachePath = "C:\nonexistent-pagespeed-cache-302\cache"
$ExpectedStatus = "cache-path-missing"

# Resolve the config file the IIS module reads at request time. Mirrors
# pagespeed/iis/iis_configuration.h FindConfigFile(): <appPhysicalPath>\
# pagespeed.config if it exists, else <appPhysicalPath>\iiswebspeed.config. The
# physical path from appcmd may contain unexpanded env vars (e.g.
# %SystemDrive%), so expand it - IIS expands it at runtime.
function Resolve-ModuleConfigPath {
    param([string]$site)
    $raw = (& $appcmd list vdir "$site/" /text:physicalPath) 2>$null
    if (-not $raw) { return $null }
    $dir = [System.Environment]::ExpandEnvironmentVariables($raw.Trim())
    $primary = Join-Path $dir "pagespeed.config"
    if (Test-Path -LiteralPath $primary) { return $primary }
    $fallback = Join-Path $dir "iiswebspeed.config"
    if (Test-Path -LiteralPath $fallback) { return $fallback }
    return $primary  # report the primary path in the not-found error below
}

function Get-PsHeaders {
    param([string]$u)
    try {
        $r = Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 5
        return $r.Headers
    } catch {
        return $null
    }
}

function Wait-ForHeader {
    param(
        [string]$u,
        [string]$name,
        [string]$expectedValue = $null,
        [bool]$expectPresent = $true,
        [int]$timeoutSec = 30
    )
    $elapsed = 0
    while ($elapsed -lt $timeoutSec) {
        $h = Get-PsHeaders -u $u
        if ($h) {
            $v = $h[$name]
            if ($expectPresent) {
                if ($v) {
                    if (-not $expectedValue -or $v -eq $expectedValue) { return $v }
                }
            } else {
                if (-not $v) { return $null }
            }
        }
        Start-Sleep -Seconds 2
        $elapsed += 2
    }
    return [string]::Empty  # sentinel: poll timed out
}

function Recycle-AppPool {
    param([string]$pool)
    # Use appcmd directly so we don't depend on the WebAdministration module
    # being importable in the test harness's shell.
    & $appcmd recycle apppool /apppool.name:$pool | Out-Null
    # Recycle is asynchronous; give the worker a moment to spin up before the
    # first poll fires a request.
    Start-Sleep -Seconds 3
}

# --- Resolve the config the module actually reads ---
if (-not $ConfigPath) {
    $ConfigPath = Resolve-ModuleConfigPath -site $SiteName
    Write-Host "Resolved module config (FindConfigFile of '$SiteName'): $ConfigPath"
}

# --- Pre-condition checks ---
Write-Host "=== Pre-conditions ==="
if (-not $ConfigPath -or -not (Test-Path -LiteralPath $ConfigPath)) {
    Write-Error "pagespeed.config not found at '$ConfigPath' (resolved from site '$SiteName' physical path; is the PageSpeed IIS module installed?)"
    exit 2
}

$baseline = Get-PsHeaders -u $Url
if (-not $baseline) {
    Write-Error "Cannot reach $Url - IIS not responding."
    exit 2
}
if (-not $baseline["X-Page-Speed"]) {
    Write-Host "::warning::Baseline did not show X-Page-Speed header. Module may already be in a failure state; continuing - the test will detect that as a pre-existing failure rather than a regression."
}
if ($baseline["X-Pagespeed-Init-Status"]) {
    Write-Error "Baseline already emits X-Pagespeed-Init-Status='$($baseline['X-Pagespeed-Init-Status'])' - module is in a failure state before the test even starts. Fix the install before running this regression guard."
    exit 2
}
Write-Host "Baseline OK: X-Page-Speed=$($baseline['X-Page-Speed']); no X-Pagespeed-Init-Status."

# --- Provision the failure mode + restore on any exit path ---
$backupPath = "$ConfigPath.regression-backup"
$originalContent = Get-Content -Raw -LiteralPath $ConfigPath
Copy-Item -LiteralPath $ConfigPath -Destination $backupPath -Force

$restored = $false
function Restore-Config {
    if ($script:restored) { return }
    Write-Host "Restoring original pagespeed.config..."
    Set-Content -LiteralPath $ConfigPath -Value $script:originalContent -NoNewline
    Recycle-AppPool -pool $AppPool
    $script:restored = $true
}
# Restore on any exit path (success, throw, Ctrl+C). trap is the PS idiom
# closest to `defer`; it runs once on the first terminating error in scope.
trap { Restore-Config; break }

try {
    # --- Provision: point FileCachePath at a bogus missing path ---
    # Rewrite the FileCachePath directive to an explicit out-of-prefix,
    # non-existent path so the module surfaces kCachePathMissing. Matches BOTH
    # the canonical "pagespeed FileCachePath ..." and the legacy
    # "ModPagespeedFileCachePath ..." spellings.
    Write-Host "=== Provisioning failure mode (FileCachePath -> $BogusCachePath) ==="
    $bogusLine = 'pagespeed FileCachePath "' + $BogusCachePath + '"'
    $patched = $originalContent -replace `
        '(?m)^\s*(?:ModPagespeed|pagespeed\s+)FileCachePath\s+.*$', `
        $bogusLine
    if ($patched -eq $originalContent) {
        # No FileCachePath directive present -> append one with the bogus path
        # so the module reads an explicit missing path rather than its default.
        $patched = $originalContent.TrimEnd() + "`n" + $bogusLine + "`n"
    }
    Set-Content -LiteralPath $ConfigPath -Value $patched -NoNewline
    Recycle-AppPool -pool $AppPool

    Write-Host "=== Asserting X-Pagespeed-Init-Status=$ExpectedStatus ==="
    $diag = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                           -expectedValue $ExpectedStatus `
                           -timeoutSec $PollSeconds
    if ($diag -eq [string]::Empty) {
        # Capture current headers for the failure message.
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw "Regression: expected X-Pagespeed-Init-Status='$ExpectedStatus' within ${PollSeconds}s; observed '$obs'."
    }
    Write-Host "PASS: header emitted with value '$diag'."

    Restore-Config

    Write-Host "=== Asserting healthy state restored ==="
    # After restore + recycle, the diagnostic header must disappear AND
    # X-Page-Speed must come back.
    $cleared = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                              -expectPresent $false -timeoutSec $PollSeconds
    if ($cleared -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw "Regression: X-Pagespeed-Init-Status did not clear within ${PollSeconds}s after restore; observed '$obs'."
    }
    $xps = Wait-ForHeader -u $Url -name "X-Page-Speed" -timeoutSec $PollSeconds
    if (-not $xps) {
        throw "Regression: X-Page-Speed did not return within ${PollSeconds}s after restore (module did not recover from the failure mode)."
    }
    Write-Host "PASS: healthy state restored (X-Page-Speed=$xps, no X-Pagespeed-Init-Status)."

    Write-Host "=== test_iis_cache_diagnostic.ps1: ALL PASS ==="
    exit 0
}
finally {
    # Belt-and-braces: trap above handles terminating errors, but a clean exit
    # path also needs to restore the config + recycle so the host is left in
    # the same state we found it. No cache-root wipe is needed: the bogus path
    # is out-of-prefix + non-existent, so the module writes nothing during the
    # failure window, and the restore re-points at the original cache.
    Restore-Config
    # Remove the on-disk backup so the host is left exactly as found (Restore-Config
    # restores from the in-memory copy, so the backup file is otherwise orphaned).
    if (Test-Path -LiteralPath $backupPath) {
        Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
    }
}

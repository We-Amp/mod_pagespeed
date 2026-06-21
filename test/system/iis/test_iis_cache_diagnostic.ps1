# test_iis_cache_diagnostic.ps1 - Regression guard for the
# X-Pagespeed-Init-Status response header emitted by
# pagespeed/iis/iis_process_context.cpp + iis_http_module.cpp when the
# module fails to initialize because the configured FileCachePath is
# missing, empty, or unwritable.
#
# The header (and the matching per-failure-mode local-only error page)
# is the diagnostic mechanism that justifies Return="ignore" on the
# GrantCacheAcl / GrantLogAcl deferred-exe CAs in install/iis/Product.wxs:
# if those CAs silently fail to grant IIS_IUSRS write on the cache dir,
# the module surfaces "cache-path-not-writable" to localhost admin on
# the very first request instead of failing the MSI mid-install. The
# whole pattern is worthless if a future regression silently drops the
# header, so this test pins the header-emission contract.
#
# Provisions a known-failure-mode (FileCachePath line DELETED, so
# file_cache_path() falls back to its empty default) by editing the
# shipped pagespeed.config, recycles the IIS app pool to force module
# re-init, asserts the header, then restores the original config and
# confirms the healthy state returns.
#
# Pre-conditions (the test asserts these and aborts cleanly if absent):
#   - PageSpeed IIS module already installed + registered
#   - Default IIS site responding on http://localhost/
#   - Healthy state: X-Page-Speed header present, no X-Pagespeed-Init-Status
#
# Run as Administrator (icacls + appcmd recycle require it).

[CmdletBinding()]
# Fresh 1.1+ MSI installs file pagespeed.config into
#   C:\ProgramData\We-Amp\PageSpeed\pagespeed.config
# alongside the cache + logs subdirectories — one canonical product
# directory. iis_module_factory.cpp resolves canonical-first with a
# runtime fallback to the legacy
#   C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config
# for upgrade-from-IISpeed and upgrade-from-1.1-legacy installs.
# Override -ConfigPath if testing against a legacy-layout install.
param(
    [string]$Url = "http://localhost/",
    [string]$ConfigPath = "C:\ProgramData\We-Amp\PageSpeed\pagespeed.config",
    [string]$AppPool = "DefaultAppPool",
    [string]$CacheRoot = "C:\ProgramData\We-Amp\PageSpeed\cache",
    [int]$PollSeconds = 30
)

$ErrorActionPreference = 'Stop'

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
    & "$env:SystemRoot\system32\inetsrv\appcmd.exe" recycle apppool /apppool.name:$pool | Out-Null
    # Recycle is asynchronous; give the worker a moment to spin up before
    # the first poll fires a request.
    Start-Sleep -Seconds 3
}

# Note on setup hygiene: an earlier revision cleared the cache root in
# setup, but on the iis-msi-upgrade-test Hyper-V VM the clear required
# a W3SVC restart to release w3wp handles, which then introduced a
# transient init-failure window where the very first baseline request
# saw the module mid-warmup with X-Pagespeed-Init-Status set (per-site
# cache subdir auto-create runs ON first request, so request #1 races
# with init). This fixture exercises kCachePathEmpty (FileCachePath="")
# which does NOT read the cache directory at all, so the clear was
# never load-bearing for THIS test's correctness. The sister fixture
# test_iis_cache_autocreate.ps1 owns the clean-cache-root contract; it
# polls Wait-ForHeader and tolerates the transient.

# --- Pre-condition checks ---
Write-Host "=== Pre-conditions ==="
if (-not (Test-Path $ConfigPath)) {
    Write-Error "pagespeed.config not found at $ConfigPath (is the PageSpeed IIS module installed?)"
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
    # --- Provision: DELETE the FileCachePath directive entirely ---
    # Preserve all other config lines; only remove the FileCachePath line.
    # Matches BOTH the canonical "pagespeed FileCachePath ..." and the
    # legacy "ModPagespeedFileCachePath ..." spellings -- the shipped
    # pagespeed.config uses the canonical form, but upgrade-from-IISpeed
    # installs may still have the legacy form.
    #
    # Why delete the line instead of setting FileCachePath ""? The IIS
    # config tokenizer (pagespeed/iis/iis_config_util.h) DROPS empty
    # quoted tokens: it only pushes a token when tmp.size() != 0, so
    # `pagespeed FileCachePath ""` tokenizes to just ["FileCachePath"]
    # (n_args == 1). iis_configuration.cpp's `tokens.size() == 2` guard
    # then never runs the path setter, and ParseAndSetOptions routes the
    # 1-arg directive to ParseAndSetOptions0 (which only knows
    # diagnose/on/off/unplugged) -> kOptionNameUnknown -> FileCachePath
    # is never set -> file_cache_path() keeps its prior/default value and
    # the module never enters kCachePathEmpty.
    #
    # Deleting the directive entirely is a genuinely reachable "admin
    # removed the line" case: with no FileCachePath directive, the
    # SystemRewriteOptions file_cache_path_ property keeps its registered
    # default of "" (pagespeed/system/system_rewrite_options.cc:93), so
    # iis_process_context.cpp:282 `if (cache_path.empty())` fires
    # InitFailureKind::kCachePathEmpty -> X-Pagespeed-Init-Status:
    # cache-path-empty (iis_http_module.cpp:490-491).
    Write-Host "=== Provisioning failure mode (FileCachePath line deleted) ==="
    # Drop the whole FileCachePath line (including its trailing newline so
    # we don't leave a blank line behind).
    $patched = $originalContent -replace `
        '(?m)^\s*(?:ModPagespeed|pagespeed\s+)FileCachePath\s+.*\r?\n?', `
        ''
    if ($patched -eq $originalContent) {
        # Directive was already absent -> the module already sees the
        # empty default; nothing to remove. The assertion below will
        # still validate kCachePathEmpty fires.
        Write-Host "::warning::No FileCachePath directive found to remove; config already has none. Proceeding - the empty default should still trip kCachePathEmpty."
    }
    Set-Content -LiteralPath $ConfigPath -Value $patched -NoNewline
    Recycle-AppPool -pool $AppPool

    Write-Host "=== Asserting X-Pagespeed-Init-Status=cache-path-empty ==="
    $diag = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                           -expectedValue "cache-path-empty" `
                           -timeoutSec $PollSeconds
    if ($diag -eq [string]::Empty) {
        # Capture current headers for the failure message.
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw "Regression: expected X-Pagespeed-Init-Status='cache-path-empty' within ${PollSeconds}s; observed '$obs'."
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
    # Belt-and-braces: trap above handles terminating errors, but a clean
    # exit path also needs to restore the config + recycle so the host is
    # left in the same state we found it.
    Restore-Config
    # Defense-in-depth: the design record §6 says the setup clear is load-bearing,
    # but a residual cache root left for the next fixture is still
    # noise. Best-effort; ignore failures.
    if (Test-Path -LiteralPath $CacheRoot) {
        Remove-Item -LiteralPath $CacheRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

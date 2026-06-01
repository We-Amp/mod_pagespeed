# test_iis_config_fallback.ps1 - Pins the canonical config-path contract for
# iis_module_factory.cpp's config-path resolution.
#
# The MSI now files pagespeed.config into the
# canonical product directory
#   C:\ProgramData\We-Amp\PageSpeed\pagespeed.config
# alongside the cache + logs subdirectories. Upgrade-from-IISpeed and
# upgrade-from-1.1-legacy customers keep their config at the legacy
#   C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config
# via NeverOverwrite="yes" on Product.wxs:228. iis_module_factory.cpp
# resolves canonical-first with a runtime fallback to the legacy path.
#
# This test proves the fallback path works: with the canonical config
# absent and a distinctive directive in the legacy config, the module
# loads the legacy config. The distinctive directive used is
# `pagespeed Statistics off`, which removes /pagespeed_statistics from
# the served endpoints -- the test asserts that endpoint 404s under the
# legacy-only state and 200s after restore.
#
# Pre-conditions (asserted; test aborts cleanly if absent):
#   - PageSpeed IIS module already installed + registered
#   - Default IIS site responding on http://localhost/
#   - Healthy state at script entry: X-Page-Speed header present, no
#     X-Pagespeed-Init-Status
#   - The canonical config file exists at the canonical path
#
# Run as Administrator (file write + appcmd recycle require it).

[CmdletBinding()]
param(
    [string]$Url = "http://localhost/",
    [string]$CanonicalConfig = "C:\ProgramData\We-Amp\PageSpeed\pagespeed.config",
    [string]$LegacyDir = "C:\ProgramData\We-Amp\IISWebSpeed",
    [string]$LegacyConfig = "C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config",
    [string]$StatsUrl = "http://localhost/pagespeed_statistics",
    [string]$AppPool = "DefaultAppPool",
    [int]$PollSeconds = 30
)

$ErrorActionPreference = 'Stop'

function Get-PsResponse {
    param([string]$u)
    try {
        return Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 5
    } catch {
        # Capture 4xx/5xx as a synthetic response with the status code.
        if ($_.Exception.Response) {
            return [pscustomobject]@{
                StatusCode = [int]$_.Exception.Response.StatusCode
                Headers    = @{}
            }
        }
        return $null
    }
}

function Get-PsHeaders {
    param([string]$u)
    $r = Get-PsResponse -u $u
    if ($r -and $r.Headers) { return $r.Headers } else { return $null }
}

function Recycle-AppPool {
    param([string]$pool)
    & "$env:windir\system32\inetsrv\appcmd.exe" recycle apppool $pool | Out-Null
    Start-Sleep -Seconds 2
}

function Wait-For {
    param(
        [scriptblock]$cond,
        [int]$timeoutSec = 30,
        [string]$desc = "condition"
    )
    $elapsed = 0
    while ($elapsed -lt $timeoutSec) {
        if (& $cond) { return $true }
        Start-Sleep -Seconds 1
        $elapsed++
    }
    Write-Error "Timed out waiting for $desc after ${timeoutSec}s"
    return $false
}

# --- Pre-condition checks ------------------------------------------------

Write-Host "[fallback-test] Pre-condition: canonical config present at $CanonicalConfig"
if (-not (Test-Path -LiteralPath $CanonicalConfig)) {
    Write-Error "Canonical config not present; cannot exercise fallback (MSI not installed, or already migrated to legacy-only)"
    exit 2
}

Write-Host "[fallback-test] Pre-condition: healthy baseline (X-Page-Speed header present)"
$baselineHeaders = Get-PsHeaders -u $Url
if (-not $baselineHeaders -or -not $baselineHeaders['X-Page-Speed']) {
    Write-Error "Baseline not healthy: X-Page-Speed header absent at $Url"
    exit 2
}
if ($baselineHeaders['X-Pagespeed-Init-Status']) {
    Write-Error "Baseline already showing X-Pagespeed-Init-Status=$($baselineHeaders['X-Pagespeed-Init-Status'])"
    exit 2
}

# --- Setup: stash canonical, plant distinctive legacy config -------------

$stash = "$CanonicalConfig.fallback-test-bak"
Write-Host "[fallback-test] Stashing canonical config -> $stash"
Copy-Item -LiteralPath $CanonicalConfig -Destination $stash -Force

try {
    # Read canonical config as the basis for the legacy variant so we
    # don't drift wildly from prod-shipped defaults -- only flip the
    # one distinctive directive.
    $legacyContent = (Get-Content -LiteralPath $CanonicalConfig -Raw) `
        -replace '(?m)^\s*pagespeed\s+Statistics\s+on\s*$', 'pagespeed Statistics off' `
        -replace '(?m)^\s*pagespeed\s+StatisticsLogging\s+on\s*$', 'pagespeed StatisticsLogging off'

    if ($legacyContent -notmatch 'pagespeed\s+Statistics\s+off') {
        Write-Error "Canonical config did not contain 'pagespeed Statistics on' -- cannot derive distinctive legacy variant"
        exit 2
    }

    if (-not (Test-Path -LiteralPath $LegacyDir)) {
        Write-Host "[fallback-test] Creating legacy dir $LegacyDir"
        New-Item -ItemType Directory -Path $LegacyDir -Force | Out-Null
    }
    Write-Host "[fallback-test] Writing distinctive legacy config -> $LegacyConfig"
    Set-Content -LiteralPath $LegacyConfig -Value $legacyContent -Encoding ASCII

    Write-Host "[fallback-test] Removing canonical config so only legacy fallback exists"
    Remove-Item -LiteralPath $CanonicalConfig -Force

    Write-Host "[fallback-test] Recycling AppPool $AppPool"
    Recycle-AppPool -pool $AppPool

    # Module re-init can take a beat -- poll the stats endpoint.
    Write-Host "[fallback-test] Asserting Statistics is off (legacy config loaded)"
    $ok = Wait-For -timeoutSec $PollSeconds -desc "Statistics off via legacy fallback" -cond {
        $r = Get-PsResponse -u $StatsUrl
        # Legacy config has `Statistics off` -- endpoint should 404.
        # (Healthy baseline: 200.)
        return ($r -and $r.StatusCode -eq 404)
    }
    if (-not $ok) {
        $r = Get-PsResponse -u $StatsUrl
        $code = if ($r) { $r.StatusCode } else { "<no response>" }
        Write-Error "Fallback did not take effect: $StatsUrl returned $code (expected 404 because legacy config has Statistics off)"
        exit 1
    }
    Write-Host "[fallback-test] PASS: legacy IISWebSpeed\pagespeed.config was loaded"

} finally {
    # --- Teardown: restore canonical, drop legacy, recycle -----------

    Write-Host "[fallback-test] Restoring canonical config from stash"
    Move-Item -LiteralPath $stash -Destination $CanonicalConfig -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $CanonicalConfig) -and (Test-Path -LiteralPath $stash)) {
        Copy-Item -LiteralPath $stash -Destination $CanonicalConfig -Force
        Remove-Item -LiteralPath $stash -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $LegacyConfig) {
        Write-Host "[fallback-test] Removing planted legacy config $LegacyConfig"
        Remove-Item -LiteralPath $LegacyConfig -Force
    }
    Write-Host "[fallback-test] Recycling AppPool $AppPool to restore healthy state"
    Recycle-AppPool -pool $AppPool
    Wait-For -timeoutSec $PollSeconds -desc "stats endpoint 200 after restore" -cond {
        $r = Get-PsResponse -u $StatsUrl
        return ($r -and $r.StatusCode -eq 200)
    } | Out-Null
}

exit 0

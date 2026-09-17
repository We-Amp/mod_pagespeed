# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# test_iis_config_fallback.ps1 - Pins the canonical-config-with-legacy-
# fallback contract for iis_module_factory.cpp's config-path resolution.
#
# The MSI now files pagespeed.config into the canonical product directory
#   C:\ProgramData\We-Amp\PageSpeed\pagespeed.config
# alongside the cache + logs subdirectories. Upgrade-from-IISpeed and
# upgrade-from-early-1.1 customers keep their config at the legacy
#   C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config
# via NeverOverwrite="yes" on Product.wxs:228. iis_module_factory.cpp
# resolves canonical-first with a runtime fallback to the legacy path.
#
# This test proves the fallback path works: with the canonical config
# absent and a distinctive directive in the legacy config, the module
# loads the legacy config. The distinctive directive used is
# `pagespeed off`: when the module is disabled it never runs PSOL
# on the response, so the X-Page-Speed version header is NOT emitted
# (the engine adds X-Page-Speed only when it actually optimizes a
# response -- see iis_http_module.cpp:1199, which short-circuits the
# response path with "pagespeed disabled" when options->enabled() is
# false, and :1219 which treats an existing X-Page-Speed header as proof
# the content was already rewritten). The test asserts X-Page-Speed is
# ABSENT under the legacy-only `pagespeed off` state and present again
# after restore. This is a far stronger "config reloaded" signal than
# the old `Statistics off -> /pagespeed_statistics 404` assertion, which
# was invalid: the IIS module never gates the stats endpoint on
# statistics_enabled() (Statistics controls collection, not endpoint
# availability), so that endpoint stayed 200 and the poll always timed
# out.
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
    # don't drift wildly from prod-shipped defaults -- only flip the one
    # distinctive directive: the master `pagespeed on` enable switch
    # becomes `pagespeed off`. When off, the module short-circuits
    # the response path and never emits the X-Page-Speed header, which is
    # the observable we assert below. The regex only matches the bare
    # master switch ("pagespeed on" on its own line) -- not multi-token
    # directives like "pagespeed Statistics on" or "pagespeed RewriteLevel
    # ...", which keep more tokens after "pagespeed".
    $legacyContent = (Get-Content -LiteralPath $CanonicalConfig -Raw) `
        -replace '(?m)^\s*pagespeed\s+on\s*$', 'pagespeed off'

    if ($legacyContent -notmatch '(?m)^\s*pagespeed\s+off\s*$') {
        Write-Error "Canonical config did not contain a bare 'pagespeed on' master switch -- cannot derive distinctive legacy variant"
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

    # Module re-init can take a beat -- poll for the X-Page-Speed header
    # to disappear. With the legacy config's `pagespeed off`, the module
    # never optimizes the response and never adds X-Page-Speed; its
    # absence is a strong "legacy config loaded" signal (the healthy
    # baseline above already confirmed the header is present with the
    # canonical config).
    Write-Host "[fallback-test] Asserting pagespeed is off (legacy config loaded -> X-Page-Speed absent)"
    $ok = Wait-For -timeoutSec $PollSeconds -desc "X-Page-Speed absent via legacy fallback (pagespeed off)" -cond {
        $h = Get-PsHeaders -u $Url
        # The request itself must succeed (a healthy 200 origin response);
        # what changes is that PSOL no longer stamps X-Page-Speed on it.
        return ($h -ne $null -and -not $h['X-Page-Speed'])
    }
    if (-not $ok) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h -and $h['X-Page-Speed']) { $h['X-Page-Speed'] } elseif ($null -eq $h) { "<no response>" } else { "<absent>" }
        Write-Error "Fallback did not take effect: $Url still reports X-Page-Speed='$obs' (expected absent because legacy config has 'pagespeed off')"
        exit 1
    }
    Write-Host "[fallback-test] PASS: legacy IISWebSpeed\pagespeed.config was loaded (X-Page-Speed absent under 'pagespeed off')"

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
    # Healthy state is restored when the canonical `pagespeed on` config
    # is loaded again and PSOL resumes stamping X-Page-Speed.
    Wait-For -timeoutSec $PollSeconds -desc "X-Page-Speed present again after restore" -cond {
        $h = Get-PsHeaders -u $Url
        return ($h -ne $null -and $h['X-Page-Speed'])
    } | Out-Null
}

exit 0

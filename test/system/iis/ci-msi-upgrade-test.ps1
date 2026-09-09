# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# === DAY-2 SNAPSHOT -- IMPORTANT FOR LOCAL REPRODUCTION ===
#
# The Hyper-V snapshot `02-iis-ready` on the rig host is INTENTIONALLY
# a day-2 state: it has a pre-installed PageSpeed module (beta.10-era)
# already baked into the image. Its purpose is to exercise the
# upgrade-over-warm-IIS path that real customers hit -- not a clean
# install. The day-2 snapshot is the ground truth for "did the MSI's
# WAS-stop + taskkill w3wp + InstallFiles sequence actually replace
# pagespeed_iis.dll on a host where w3wp is holding a handle to it?"
#
# DO NOT "clean" the snapshot. If you are reproducing this locally
# on a developer box without the day-2 snapshot:
#   - flow-a (clean install) still works against a vanilla IIS VM.
#   - flow-b (warm upgrade) needs you to either (a) bake a day-2
#     snapshot yourself by installing an older MSI first and then
#     checkpointing, or (b) accept that the "baseline probe" below
#     will report "No pre-existing install".
#
# The upgrade sequence being exercised: the MSI stops the WAS service
# (w3wp's parent in the IIS process tree) and issues a defensive taskkill
# of w3wp before InstallFiles, so nothing is holding a handle to the DLL
# when it is replaced.
#
# === END DAY-2 SNAPSHOT NOTE ===
#
# ci-msi-upgrade-test.ps1 - Hyper-V VM MSI lifecycle test for CI
# Runs flow-a: clean install -> smoke -> uninstall -> verify clean removal.
# When -OldMsiPath is supplied, runs flow-b first: install OLD -> iisreset ->
# warm w3wp via HTTP -> install NEW (over warm IIS) -> smoke -> uninstall.
# Called from release.yml iis-msi-upgrade-test job.
#
# Parameters:
#   -MsiPath                 Path to the MSI to test (on the host)
#   -OldMsiPath              Optional path to a synthetic-old MSI on the host.
#                            When supplied, runs flow-b (warm upgrade) instead
#                            of flow-a (clean install). Empty = flow-a.
#   -VMName                  Hyper-V VM name of the Windows Server 2016 IIS base image (required; CI passes it explicitly)
#   -Snapshot                Checkpoint to restore (default: 02-iis-ready)
#   -ReleaseTag              Expected release tag (e.g. v1.1.0-beta.7); defaults
#                            to $env:RELEASE_TAG. Used for admin-SPA assertion.
#   -ExpectedVersionString   MAJOR.MINOR.BUILD[-PRERELEASE] expected in the
#                            X-Page-Speed response header - matches the shape
#                            kModPagespeedVersion emits via
#                            MOD_PAGESPEED_FULL_VERSION_STRING in
#                            net/instaweb/public/version.h.in. Derived from
#                            net/instaweb/public/VERSION if not supplied.

param(
    [Parameter(Mandatory=$true)]
    [string]$MsiPath,

    # Optional: when non-empty, install this OLD MSI first, iisreset, warm a
    # worker via HTTP, then install the NEW MSI on top. Reproduces the warm-IIS
    # upgrade scenario that locked up beta.9. Empty -> flow-a (clean install).
    [string]$OldMsiPath = "",

    [string]$VMName = "",

    [string]$Snapshot = "02-iis-ready",

    # Release tag we expect the MSI to identify itself as (e.g. v1.1.0-beta.7).
    # Defaults to RELEASE_TAG env so CI workflows can plumb it through without
    # extra wiring; an empty value disables the admin-HTML tag assertion.
    [string]$ReleaseTag = $env:RELEASE_TAG,

    # Version string we expect to find inside the X-Page-Speed header: shape is
    # MAJOR.MINOR.BUILD optionally followed by "-PRERELEASE" (e.g. 1.1.0-beta.7
    # or 1.1.0 for a stable release). If empty, derive from ReleaseTag by
    # stripping the leading 'v' (e.g. "v1.1.0-beta.10" -> "1.1.0-beta.10").
    [string]$ExpectedVersionString = ""
)

$ErrorActionPreference = 'Stop'

# --- Derive ExpectedVersionString from ReleaseTag if not supplied ---
# The local checkout's VERSION file lives at HEAD of master (e.g.
# PRERELEASE=beta.1 even on a beta.10 build) because the release-time
# patching happens inside the build tarball, not in the repo. ReleaseTag
# is the only reliable source of truth for "what version was this MSI
# built as" from the test job's perspective.
if ([string]::IsNullOrEmpty($ExpectedVersionString) -and -not [string]::IsNullOrEmpty($ReleaseTag)) {
    # Strip leading 'v' and any semver build-metadata suffix ('+...'); the
    # latter is per semver 2.0.0 not part of version identity (e.g. a reship
    # tag like 'v1.1.0+r1' encodes provenance, not a different binary
    # version, so the X-Page-Speed header still reports '1.1.0').
    $ExpectedVersionString = ($ReleaseTag -replace '^v', '') -replace '\+[^+]+$', ''
    Write-Host "Derived ExpectedVersionString from ReleaseTag: $ExpectedVersionString"
}
Write-Host "ReleaseTag: '$ReleaseTag'"
Write-Host "ExpectedVersionString: '$ExpectedVersionString'"

# --- Helpers ---
function Restore-CleanState {
    Write-Host "Restoring VM to clean state..."
    Stop-VM -Name $VMName -Force -ErrorAction SilentlyContinue
    Restore-VMSnapshot -VMName $VMName -Name $Snapshot -Confirm:$false -ErrorAction SilentlyContinue
}

trap { Restore-CleanState }

# --- Validate inputs ---
if (-not (Test-Path $MsiPath)) {
    Write-Error "MSI not found: $MsiPath"
    exit 1
}
# Only validate OldMsiPath when non-empty: PowerShell Test-Path "" returns
# $false, so a naked Test-Path on an empty string would trip the error path.
if ($OldMsiPath -and -not (Test-Path $OldMsiPath)) {
    Write-Error "Old MSI not found: $OldMsiPath"
    exit 1
}

$msiName = Split-Path $MsiPath -Leaf
$oldMsiName = if ($OldMsiPath) { Split-Path $OldMsiPath -Leaf } else { "" }
$adminPass = ConvertTo-SecureString 'Hv!PassThr0w' -AsPlainText -Force
$cred = New-Object System.Management.Automation.PSCredential("Administrator", $adminPass)

# --- Step 1: Restore VM to clean IIS-ready state ---
Write-Host "=== Restoring VM '$VMName' to checkpoint '$Snapshot' ==="
Restore-VMSnapshot -VMName $VMName -Name $Snapshot -Confirm:$false
Start-VM -Name $VMName

# Wait for VM heartbeat (integration services ready)
$timeout = 120
$elapsed = 0
while ($elapsed -lt $timeout) {
    $hb = (Get-VM $VMName).Heartbeat
    if ($hb -eq 'OkApplicationsHealthy') { break }
    Start-Sleep -Seconds 3
    $elapsed += 3
}
if ((Get-VM $VMName).Heartbeat -ne 'OkApplicationsHealthy') {
    Write-Error "VM did not become ready within ${timeout}s"
    exit 1
}
Write-Host "VM ready (heartbeat OK, ${elapsed}s)"

# --- Baseline probe: day-2 upgrade-from-state visibility ---
Write-Host "=== Probing day-2 baseline ==="
$baselineScript = {
    $dll = "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll"
    if (Test-Path $dll) {
        try {
            $vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($dll)
            $ver = if ($vi.ProductVersion) { $vi.ProductVersion } else { $vi.FileVersion }
            if (-not $ver) { $ver = '<pre-beta.20 install -- no embedded VS_VERSION_INFO>' }
            $size = (Get-Item $dll).Length
            Write-Host "[BASELINE] Pre-installed PageSpeed: $ver (DLL $size bytes at $dll)"
        } catch {
            Write-Host "[BASELINE] Pre-installed PageSpeed DLL present but version unreadable: $($_.Exception.Message)"
        }
        $cfgPath = "$env:SystemRoot\system32\inetsrv\config\applicationHost.config"
        if ((Test-Path $cfgPath) -and ((Get-Content $cfgPath -Raw) -match 'PageSpeedModule')) {
            Write-Host "[BASELINE] PageSpeedModule already registered in applicationHost.config"
        } else {
            Write-Host "[BASELINE] DLL present but PageSpeedModule NOT registered (orphaned file)"
        }
    } else {
        Write-Host "[BASELINE] No pre-existing install (clean IIS - flow-b will install OLD to create warm state)"
    }
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $baselineScript

# --- Step 2: Prepare artifacts directory in VM ---
Write-Host "Preparing artifacts staging in VM..."
$prepScript = {
    New-Item -ItemType Directory -Path "C:\artifacts" -Force | Out-Null
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $prepScript

# --- Step 3: Copy MSI(s) + port-parity fixtures into VM ---
Write-Host "Copying MSI to VM..."
$session = New-PSSession -VMName $VMName -Credential $cred
Copy-Item -Path $MsiPath -Destination "C:\artifacts\$msiName" -ToSession $session -Force
if ($OldMsiPath) {
    Write-Host "Copying old MSI to VM ($oldMsiName)..."
    Copy-Item -Path $OldMsiPath -Destination "C:\artifacts\$oldMsiName" -ToSession $session -Force
}

# Copy port-level fixtures into VM. These run AFTER MSI install
# (Step 5b below) against the freshly-installed module to pin the auto-
# create + diagnostic-page + config-fallback contracts on each release.
#
# Fixture gating status (live-VM verified 2026-06-22 on the Windows Server 2016 IIS base image):
#   - test_iis_cache_diagnostic.ps1: GATING. Fixed + VM-validated. It
#     now edits the config the MODULE actually reads at request time
#     (FindConfigFile(<site physical path>) = ...\wwwroot\pagespeed.config,
#     NOT the ProgramData copy the old fixture edited) and provisions a
#     REACHABLE failure: an explicit out-of-auto-create-prefix, non-existent
#     FileCachePath -> X-Pagespeed-Init-Status: cache-path-missing. (Verified
#     on the VM that `FileCachePath ""`, deleting the directive, AND emptying
#     the whole file all FAIL to trip a failure mode -- the module falls back
#     to a working default cache path -- so cache-path-empty is unreachable
#     via a config edit; cache-path-missing is the reliable contract.)
#   - test_iis_cache_autocreate.ps1 + test_iis_logdir_autocreate.ps1:
#     GATING. Fixed + VM-validated 2026-06-22. autocreate was REWRITTEN for the
#     CYCLONE cache (assert cache root + cyclone.dat auto-init + healthy, plus a
#     cache-path-missing negative; the old per-site-subdir/ACL + create-failed
#     assertions tested the obsolete pre-Cyclone file-cache). logdir keeps its
#     positive LogDir auto-create contract; its log-dir-create-failed negative is
#     RETIRED (a denied LogDir is non-fatal on v1.15.0 — VM-verified).
#   - test_iis_config_fallback.ps1: still NON-GATING. It tests the
#     ProgramData canonical->legacy FACTORY fallback, but request-time options
#     come from FindConfigFile(<wwwroot>) — entangled with the config-resolution
#     duality; fix once that settles which config governs what.
$portFixtures = @(
    'test_iis_cache_diagnostic.ps1',
    'test_iis_cache_autocreate.ps1',
    'test_iis_logdir_autocreate.ps1',
    'test_iis_config_fallback.ps1'
)
$portFixtureDir = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) ''
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock {
    New-Item -ItemType Directory -Path 'C:\artifacts\port-fixtures' -Force | Out-Null
}
foreach ($f in $portFixtures) {
    $src = Join-Path $portFixtureDir $f
    if (Test-Path -LiteralPath $src) {
        Copy-Item -Path $src -Destination "C:\artifacts\port-fixtures\$f" -ToSession $session -Force
        Write-Host "  port fixture staged: $f"
    } else {
        Write-Host "::warning::port fixture not found at $src - skipping"
    }
}
Remove-PSSession $session

# --- Step 4a (flow-b only): Install OLD MSI, iisreset, warm a w3wp worker ---
# Reproduces the legacy beta.9 scenario: the OLD product is running with a
# worker holding pagespeed_iis.dll mmap'd when the NEW MSI tries to replace
# the file. Without this, flow-a tests a cold-IIS install which never hits
# the file-in-use path.
if ($OldMsiPath) {
    Write-Host "=== Flow-B: Warm upgrade (install OLD, warm worker, install NEW) ==="
    $warmScript = {
        param($oldMsi)
        $log = "C:\artifacts\install-old.log"
        $proc = Start-Process msiexec.exe -ArgumentList "/i `"C:\artifacts\$oldMsi`" /qn /l*v `"$log`"" -Wait -PassThru
        if ($proc.ExitCode -ne 0) {
            Get-Content $log -Tail 50 | Write-Host
            throw "OLD MSI install failed with exit code $($proc.ExitCode)"
        }
        Write-Host "OLD MSI installed OK"

        # iisreset to ensure a clean worker pool that will load the freshly
        # registered PageSpeedModule when the first request lands.
        & iisreset /restart 2>&1 | Out-Null
        Start-Sleep -Seconds 5

        # Warm a w3wp.exe worker by issuing an HTTP request. The goal is to
        # get a worker process started with pagespeed_iis.dll mmap'd; we do
        # NOT need a successful response. Fail-soft: warn and proceed.
        $warmed = $false
        for ($i = 1; $i -le 15; $i++) {
            try {
                Invoke-WebRequest -Uri "http://localhost/" -UseBasicParsing -TimeoutSec 5 | Out-Null
                $warmed = $true
                break
            } catch {
                # IIS may still be starting; retry
            }
            Start-Sleep -Seconds 2
        }
        if ($warmed) {
            Write-Host "Warmed w3wp worker via HTTP"
        } else {
            Write-Host "::warning::IIS did not respond within warm-up budget; proceeding anyway (worker may still be loaded)"
        }
    }
    Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $warmScript -ArgumentList $oldMsiName
}

# Defensive: strip any pre-existing IIS_IUSRS / NETWORK SERVICE ACL on the
# cache + logs dirs before install, so the smoke proves the new MSI
# restores writability via its own ACL logic (util:PermissionEx +
# GrantCacheAcl/GrantLogAcl). Without this, a stale ACL from a prior
# beta.10-era install (which created the dirs without explicit grants
# but left them writable via inheritance from ProgramData's default
# DACL) could mask a CacheDirComp/PermissionEx regression. Idempotent
# no-op when paths don't exist (Test-Path guard). Applied in BOTH
# flow-a and flow-b because flow-b's OldMsiPath install would
# otherwise leave a stale ACL behind that masks the same regression.
Write-Host "Stripping any pre-existing IIS_IUSRS ACL on cache + logs dirs (defensive)..."
$aclStripScript = {
    foreach ($p in @(
        "C:\ProgramData\We-Amp\PageSpeed\cache",
        "C:\ProgramData\We-Amp\PageSpeed\logs"
    )) {
        if (Test-Path $p) {
            icacls $p /remove:g "*S-1-5-32-568" /T 2>&1 | Out-Null
            icacls $p /remove:g "*S-1-5-20"     /T 2>&1 | Out-Null
        }
    }
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $aclStripScript

# --- Step 4: Install NEW MSI ---
# In flow-a this is a clean install over fresh-checkpoint IIS.
# In flow-b this is an upgrade over warm IIS (worker holding old DLL).
if ($OldMsiPath) {
    Write-Host "=== Installing NEW MSI over warm IIS (upgrade path) ==="
} else {
    Write-Host "=== Flow-A: Clean install ==="
}
$installScript = {
    param($msi)
    $log = "C:\artifacts\install.log"
    $proc = Start-Process msiexec.exe -ArgumentList "/i `"C:\artifacts\$msi`" /qn /l*v `"$log`"" -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        Get-Content $log -Tail 50 | Write-Host
        throw "MSI install failed with exit code $($proc.ExitCode)"
    }
    # Use Get-WebGlobalModule (checks applicationHost.config) - appcmd list module
    # only shows modules loaded in running pools, which requires IIS restart first
    Import-Module WebAdministration -ErrorAction SilentlyContinue
    $mod = Get-WebGlobalModule -Name 'PageSpeedModule' -ErrorAction SilentlyContinue
    if (-not $mod) {
        # Fallback: check applicationHost.config directly
        $cfg = Get-Content "$env:SystemRoot\system32\inetsrv\config\applicationHost.config" -Raw
        if ($cfg -notmatch "PageSpeed") {
            Get-Content $log -Tail 30 | Write-Host
            throw "PageSpeedModule not registered in IIS after install"
        }
    }
    Write-Host "PageSpeedModule registered OK"

    # The license text and the attribution notices install next to the module
    # (Apache-2.0 terms). Missing or empty is a packaging regression.
    $installDir = "C:\Program Files\We-Amp\PageSpeed"
    foreach ($f in @('LICENSE', 'NOTICE')) {
        $p = Join-Path $installDir $f
        if (-not (Test-Path -LiteralPath $p)) {
            Get-Content $log -Tail 30 | Write-Host
            throw "$f not installed at $p (the MSI must ship LICENSE and NOTICE alongside pagespeed_iis.dll)"
        }
        if ((Get-Item -LiteralPath $p).Length -eq 0) {
            throw "$p is empty"
        }
    }
    if (-not (Select-String -LiteralPath (Join-Path $installDir 'LICENSE') -Pattern 'Apache License' -Quiet)) {
        throw "$installDir\LICENSE is not the Apache License text"
    }
    Write-Host "LICENSE + NOTICE installed alongside the module"
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $installScript -ArgumentList $msiName

# --- Step 5: Smoke test (strict) ---
# The module always optimizes (there is no license state), so both probes
# below are HARD asserts: the X-Page-Speed header must appear within the poll
# window and the admin HTML must be served. A missing header after a clean
# MSI install is a real regression (module not registered, w3wp not loading
# the DLL, init failure without a diagnostic header) and fails the gate; it
# is no longer downgraded to a warning.
Write-Host "Smoke testing PageSpeed header + admin page (strict)..."
$smokeScript = {
    param($expectedTag, $expectedVersion)
    & iisreset /restart 2>&1 | Out-Null
    Start-Sleep -Seconds 5

    # --- Cache-ACL hard-fail probe ---
    # Before the header poll, check for the
    # X-Pagespeed-Init-Status=cache-path-not-writable diagnostic header.
    # This is the loud signal emitted by iis_process_context.cpp when the
    # MSI's GrantCacheAcl deferred-exe CA failed to grant IIS_IUSRS
    # write on C:\ProgramData\We-Amp\PageSpeed\cache. Checking it first
    # turns a bare "X-Page-Speed absent" failure into the actionable
    # cache-ACL diagnosis instead of a generic module-not-running error.
    # Probe both / and /pagespeed_admin/ - the diagnostic page is
    # served local-only on the same handler.
    $diagHdr = $null
    foreach ($probePath in @('/', '/pagespeed_admin/')) {
        try {
            $r = Invoke-WebRequest -Uri "http://localhost$probePath" -UseBasicParsing -TimeoutSec 5 -ErrorAction SilentlyContinue
            $h = $r.Headers["X-Pagespeed-Init-Status"]
            if ($h) { $diagHdr = $h; break }
        } catch {
            # Diagnostic page also flows through HTTP error codes; the
            # header is on the response regardless. Best-effort.
        }
    }
    if ($diagHdr -eq "cache-path-not-writable") {
        throw "MSI install failed to ACL the cache dir: module reports X-Pagespeed-Init-Status=cache-path-not-writable. GrantCacheAcl deferred-exe CA in Product.wxs did not grant IIS_IUSRS write on C:\ProgramData\We-Amp\PageSpeed\cache. Inspect C:\artifacts\install.log."
    }
    if ($diagHdr) {
        # Other failure kinds (cache-path-empty, cache-path-missing) are
        # also diagnostic-page states the MSI smoke must not silently
        # ignore. Hard-fail with the observed value so the operator gets
        # the actionable kind.
        throw "Module init failed post-install: X-Pagespeed-Init-Status=$diagHdr. Inspect C:\artifacts\install.log + module init logs."
    }

    # --- Header poll ---
    $hdr = $null
    for ($i = 1; $i -le 15; $i++) {
        try {
            $resp = Invoke-WebRequest -Uri "http://localhost/" -UseBasicParsing -TimeoutSec 5
            $hdr = $resp.Headers["X-Page-Speed"]
            if ($hdr) { break }
        } catch {
            # retry
        }
        Start-Sleep -Seconds 2
    }

    if (-not $hdr) {
        throw "X-Page-Speed header not present on http://localhost/ after 15 attempts. The module always optimizes, so a missing header after install means it is not running (not registered, DLL not loaded, or init failed without a diagnostic header). Inspect C:\artifacts\install.log + module init logs."
    }

    Write-Host "X-Page-Speed header: $hdr"
    # Hard-assert the version string.
    # kModPagespeedVersion shape: MAJOR.MINOR.BUILD[-PRERELEASE]
    # e.g. "1.1.0-beta.10" or "1.1.0" for a stable release.
    if ($expectedVersion -and ($hdr -notmatch [regex]::Escape($expectedVersion))) {
        throw "X-Page-Speed header '$hdr' does not contain expected version string '$expectedVersion'"
    }

    # --- Admin HTML check ---
    $adminBody = $null
    $adminErr = $null
    foreach ($path in @('/pagespeed_admin/', '/pagespeed_global_admin/')) {
        try {
            $r = Invoke-WebRequest -Uri "http://localhost$path" -UseBasicParsing -TimeoutSec 5
            if ($r.Content) { $adminBody = $r.Content; break }
        } catch {
            # try next path
            $adminErr = $_.Exception.Message
        }
    }
    if (-not $adminBody) {
        throw "Could not fetch /pagespeed_admin/ or /pagespeed_global_admin/ although X-Page-Speed is present. The admin handler must serve HTML on a fresh install. Last error: $adminErr"
    }
    if ($expectedTag) {
        if ($adminBody -notmatch [regex]::Escape($expectedTag)) {
            $snippet = $adminBody.Substring(0, [Math]::Min(400, $adminBody.Length))
            throw "Admin HTML does not contain expected release tag '$expectedTag'. First 400 chars: $snippet"
        }
        Write-Host "Admin HTML contains release tag '$expectedTag'"
    } else {
        Write-Host "(ReleaseTag empty - skipping admin HTML tag assertion)"
    }

    # Note: pagespeed_iis.dll has no embedded VS_VERSION_INFO resource; nothing to assert on the file directly.
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $smokeScript `
    -ArgumentList $ReleaseTag, $ExpectedVersionString
Write-Host "Smoke step complete"

# --- Step 5b: port-parity fixtures ---
# Each fixture pins one contract surface (cache autocreate, log autocreate,
# config canonical+fallback path resolution, diagnostic-page failure-kind
# coverage). Run in sequence inside the VM against the freshly-installed
# module. NON-GATING: a fixture's non-zero exit emits a ::warning:: rather
# than failing the gating MSI-upgrade job. These fixtures were
# re-wired as hard gates once and proved fragile on the warm day-2 upgrade VM
# (e.g. the cache-diagnostic fixture deletes FileCachePath + recycles the
# AppPool, which does not reliably force a config re-read inside the poll
# window). The core install/register/header/uninstall
# assertions above still gate. The fixtures clear their own state in setup
# rather than teardown, and restore pagespeed.config on teardown.
Write-Host ""
Write-Host "=== Step 5b: port-parity fixtures ==="
$portFixtureScript = {
    param($spec)
    $fixtures = @($spec.fixtures)
    $gating   = [bool]$spec.gating
    # Continue (not Stop): a fixture's native stderr / non-zero exit must NOT raise
    # a terminating NativeCommandError (Windows PowerShell 5.1 behaviour under
    # 'Stop') before the per-fixture handling runs. For GATING fixtures we capture
    # the exit code and then `throw` (which, with the host's EAP=Stop, runs
    # Restore-CleanState and fails the job — verified); non-gating fixtures warn.
    $ErrorActionPreference = 'Continue'
    $failed = @()
    foreach ($f in $fixtures) {
        $path = "C:\artifacts\port-fixtures\$f"
        if (-not (Test-Path -LiteralPath $path)) {
            if ($gating) { throw "GATING port fixture missing on VM: $path" }
            Write-Host "::warning::fixture missing on VM: $path - skipping"
            continue
        }
        Write-Host "--- Running $f (gating=$gating) ---"
        # 2>&1 | Out-Host merges the child's stderr into the success stream so a
        # failing fixture never produces an error record; try/catch is a final net.
        $code = 1
        try {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $path 2>&1 | Out-Host
            $code = $LASTEXITCODE
        } catch {
            if ($gating) { throw "GATING port fixture $f raised: $($_.Exception.Message)" }
            Write-Host "::warning::port fixture $f raised: $($_.Exception.Message) (non-gating)"
            $failed += $f
            continue
        }
        if ($code -ne 0) {
            if ($gating) { throw "GATING port fixture $f FAILED with exit code $code" }
            Write-Host "::warning::port fixture $f failed with exit code $code (non-gating)"
            $failed += $f
            continue
        }
        Write-Host "--- $f PASSED ---"
        Write-Host ""
    }
    if ($failed.Count -gt 0) {
        Write-Host "::warning::port fixtures with non-zero exit (non-gating): $($failed -join ', ')"
    }
}
# These fixtures are GATING (fixed + VM-validated). config_fallback
# stays non-gating pending the config-resolution duality work.
$gatingFixtures    = @(
    'test_iis_cache_diagnostic.ps1',
    'test_iis_cache_autocreate.ps1',
    'test_iis_logdir_autocreate.ps1'
)
$nonGatingFixtures = @($portFixtures | Where-Object { $gatingFixtures -notcontains $_ })
# Wrap the GATING call in its own try/catch: on a gating failure we want a clean,
# immediate fail with a clear message - NOT a cascade. The script-scope trap has
# no `break`, so without this the gating throw would revert the VM and then resume
# into the non-gating fixtures + Step 6 uninstall against the just-reverted VM,
# burying the real "GATING port fixture FAILED" message under revert/uninstall
# noise. Catch -> Restore-CleanState -> exit 1 short-circuits that.
try {
    Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $portFixtureScript `
        -ArgumentList @{ fixtures = $gatingFixtures; gating = $true }
} catch {
    Write-Host "::error::GATING IIS port fixture failed: $($_.Exception.Message)"
    Restore-CleanState
    exit 1
}
if ($nonGatingFixtures.Count -gt 0) {
    Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $portFixtureScript `
        -ArgumentList @{ fixtures = $nonGatingFixtures; gating = $false }
}
Write-Host "Port-parity fixtures complete (GATING: cache-diagnostic, cache-autocreate, logdir; NON-gating: config_fallback)."

# --- Step 6: Uninstall ---
Write-Host "Uninstalling..."
$uninstallScript = {
    param($msi)
    $proc = Start-Process msiexec.exe -ArgumentList "/x `"C:\artifacts\$msi`" /qn" -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "MSI uninstall failed with exit code $($proc.ExitCode)"
    }
    # Verify module removed from applicationHost.config
    Import-Module WebAdministration -ErrorAction SilentlyContinue
    $mod = Get-WebGlobalModule -Name 'PageSpeedModule' -ErrorAction SilentlyContinue
    if ($mod) {
        throw "PageSpeedModule still registered after uninstall"
    }
    if (Test-Path "C:\Program Files\We-Amp\PageSpeed\pagespeed_iis.dll") {
        throw "pagespeed_iis.dll still present after uninstall"
    }
    foreach ($f in @('LICENSE', 'NOTICE')) {
        if (Test-Path -LiteralPath "C:\Program Files\We-Amp\PageSpeed\$f") {
            throw "$f still present after uninstall"
        }
    }
    Write-Host "Clean uninstall verified"
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $uninstallScript -ArgumentList $msiName

if ($OldMsiPath) {
    Write-Host "=== Flow-B PASSED (warm upgrade: OLD -> warm -> NEW -> smoke -> uninstall) ==="
} else {
    Write-Host "=== Flow-A PASSED ==="
}

# --- Cleanup: restore to clean state ---
Restore-CleanState
Write-Host "VM restored to clean state"

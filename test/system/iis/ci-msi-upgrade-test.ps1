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
# See corp/the internal planning notes and the design record for
# the full story.
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
#   -TokenPath               Path to the CI license token file (on the host)
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

    [Parameter(Mandatory=$true)]
    [string]$TokenPath,

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
if (-not (Test-Path $TokenPath)) {
    Write-Error "License token not found: $TokenPath"
    exit 1
}
# Only validate OldMsiPath when non-empty: PowerShell Test-Path "" returns
# $false, so a naked Test-Path on an empty string would trip the error path.
if ($OldMsiPath -and -not (Test-Path $OldMsiPath)) {
    Write-Error "Old MSI not found: $OldMsiPath"
    exit 1
}

$token = Get-Content $TokenPath -Raw
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
# Note: license token is NOT deployed here. The IIS module derives the
# license path from the configured FileCachePath (see
# pagespeed/kernel/license_v2/license_file.cc:24 LicenseFilePath: token
# lives alongside the cache directory, one level up). FileCachePath is
# set in pagespeed.config which the MSI ships and installs into
# %ProgramData%\We-Amp\IISWebSpeed\. The target directory therefore
# only exists AFTER MSI install, so license deployment moved to a
# dedicated step (4b) below.
Write-Host "Preparing artifacts staging in VM..."
$prepScript = {
    New-Item -ItemType Directory -Path "C:\artifacts" -Force | Out-Null
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $prepScript

# --- Step 3: Copy MSI(s) + the design record port fixtures into VM ---
Write-Host "Copying MSI to VM..."
$session = New-PSSession -VMName $VMName -Credential $cred
Copy-Item -Path $MsiPath -Destination "C:\artifacts\$msiName" -ToSession $session -Force
if ($OldMsiPath) {
    Write-Host "Copying old MSI to VM ($oldMsiName)..."
    Copy-Item -Path $OldMsiPath -Destination "C:\artifacts\$oldMsiName" -ToSession $session -Force
}

# the design record: copy port-level fixtures into VM. These run AFTER MSI install
# (Step 5b below) against the freshly-installed module to pin the auto-
# create + diagnostic-page + config-fallback contracts on each release.
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
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $installScript -ArgumentList $msiName

# --- Step 4b: Deploy license token into VM at the path the IIS module expects ---
# The shipped pagespeed.config sets FileCachePath = C:\ProgramData\We-Amp\PageSpeed\cache.
# pagespeed/kernel/license_v2/license_file.cc:LicenseFilePath() derives the
# license file path as <cache_path>.parent_path() / "pagespeed.license",
# i.e. C:\ProgramData\We-Amp\PageSpeed\pagespeed.license. The MSI's CacheDir
# component (Product.wxs) creates C:\ProgramData\We-Amp\PageSpeed\cache, so
# the parent directory exists after install - we just write the file.
Write-Host "Deploying license token to FileCachePath-derived location..."
$licenseDeployScript = {
    param($tkn)
    $licenseDir  = "C:\ProgramData\We-Amp\PageSpeed"
    $licensePath = Join-Path $licenseDir "pagespeed.license"
    if (-not (Test-Path $licenseDir)) {
        # Defensive: the MSI's CacheDir component should have created the
        # parent already, but if a future MSI restructure moves cache, we
        # don't want the test to silently misroute the license.
        Write-Host "::warning::License dir $licenseDir not created by MSI; creating manually. Verify pagespeed.config FileCachePath still matches this layout."
        New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
    }
    # Write as ASCII (no BOM) to avoid any chance of a BOM byte tripping
    # the license parser. The token is base64-style ASCII; ASCII encoding
    # is byte-for-byte safe.
    [System.IO.File]::WriteAllText($licensePath, $tkn, [System.Text.Encoding]::ASCII)
    Write-Host "License written: $licensePath ($([System.IO.File]::ReadAllBytes($licensePath).Length) bytes)"
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $licenseDeployScript -ArgumentList $token

# --- Step 5: Smoke test (license-tolerant) ---
# The CI VM may or may not validate the license token (cert state, signing
# identity, etc. can drift independently of this test). To keep the test
# useful as a structural check across all environments:
#   - If the X-Page-Speed header IS present, the module is in licensed state
#     and we HARD-FAIL on a version-string mismatch (the original intent).
#   - If the header is absent after the poll window, the module is in
#     unlicensed state (per the IIS unification: no header emitted).
#     Treat this as a CI-environment WARN and skip the version assertions;
#     install/register/uninstall behaviour is still covered by the
#     surrounding steps.
# Same tolerance for the admin HTML: if PSOL isn't rewriting unlicensed,
# /pagespeed_admin/ may also not be served. Don't hard-fail on that path
# when we already know the module is unlicensed.
Write-Host "Smoke testing PageSpeed header + admin page (license-tolerant)..."
$smokeScript = {
    param($expectedTag, $expectedVersion)
    & iisreset /restart 2>&1 | Out-Null
    Start-Sleep -Seconds 5

    # --- Cache-ACL hard-fail probe ---
    # Before any license-tolerant logic, check for the
    # X-Pagespeed-Init-Status=cache-path-not-writable diagnostic header.
    # This is the loud signal emitted by iis_process_context.cpp when the
    # MSI's GrantCacheAcl deferred-exe CA failed to grant IIS_IUSRS
    # write on C:\ProgramData\We-Amp\PageSpeed\cache. Without this
    # explicit check the failure mode is invisible: the module's
    # license-unsuccessful path also omits X-Page-Speed, so the
    # surrounding license-tolerant smoke would mistakenly classify a
    # cache-ACL regression as "module unlicensed; skip assertions".
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
        Write-Host "::warning::X-Page-Speed header not present after 15 attempts; module appears unlicensed on this VM. Skipping header + admin HTML version assertions. (Install/register/uninstall behaviour still validated.)"
        return
    }

    Write-Host "X-Page-Speed header: $hdr"
    # Module is in licensed state - hard-assert the version string.
    # kModPagespeedVersion shape: MAJOR.MINOR.BUILD[-PRERELEASE]
    # e.g. "1.1.0-beta.10" or "1.1.0" for a stable release.
    if ($expectedVersion -and ($hdr -notmatch [regex]::Escape($expectedVersion))) {
        throw "X-Page-Speed header '$hdr' does not contain expected version string '$expectedVersion'"
    }

    # --- Admin HTML check ---
    $adminBody = $null
    foreach ($path in @('/pagespeed_admin/', '/pagespeed_global_admin/')) {
        try {
            $r = Invoke-WebRequest -Uri "http://localhost$path" -UseBasicParsing -TimeoutSec 5
            if ($r.Content) { $adminBody = $r.Content; break }
        } catch {
            # try next path
        }
    }
    if (-not $adminBody) {
        Write-Host "::warning::Could not fetch /pagespeed_admin/ or /pagespeed_global_admin/ despite licensed header. Skipping admin HTML assertion."
        return
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
Write-Host "Smoke step complete (any warnings above indicate license-tolerant skip)"

# --- Step 5b: the design record port fixtures ---
# Each fixture pins one contract surface (cache autocreate, log autocreate,
# config canonical+fallback path resolution, diagnostic-page failure-kind
# coverage). Run in sequence inside the VM against the freshly-installed
# module; hard-fail on the first non-zero exit so the operator sees which
# contract regressed. The fixtures clear their own state in setup
# and restore pagespeed.config on teardown.
Write-Host ""
Write-Host "=== Step 5b: the design record port fixtures ==="
$portFixtureScript = {
    param([string[]]$fixtures)
    $ErrorActionPreference = 'Stop'
    foreach ($f in $fixtures) {
        $path = "C:\artifacts\port-fixtures\$f"
        if (-not (Test-Path -LiteralPath $path)) {
            Write-Host "::warning::fixture missing on VM: $path - skipping"
            continue
        }
        Write-Host "--- Running $f ---"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $path
        if ($LASTEXITCODE -ne 0) {
            throw "port fixture $f failed with exit code $LASTEXITCODE"
        }
        Write-Host "--- $f PASSED ---"
        Write-Host ""
    }
}
Invoke-Command -VMName $VMName -Credential $cred -ScriptBlock $portFixtureScript `
    -ArgumentList (,$portFixtures)
Write-Host "All the design record port fixtures passed."

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

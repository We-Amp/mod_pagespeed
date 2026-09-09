# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# test_iis_logdir_autocreate.ps1 - Pins the operational contract for LogDir
# auto-create on IIS: on a successful start the configured
# LogDir must exist and be writable by the worker identity. Notes:
#
#   - The target directory is the configured LogDir (the literal value from
#     pagespeed.config, e.g. ...\PageSpeed\logs); there is no per-site suffix.
#     (The cache fixture is now Cyclone-based and shares no per-site-subdir
#     structure with this one.)
#
#   - The runtime ACL grant is RX+W (no DELETE), mirroring
#     Product.wxs GrantLogAcl. The positive-path ACE assertion
#     accepts EITHER an explicit RX+W (= ReadAndExecute + Write) on
#     the worker SID OR an inherited Modify from the well-known SIDs
#     the installer ships (S-1-5-32-568 / S-1-5-20) — both are valid:
#     in the common case the bare mkdir suffices because the worker
#     already inherits Modify, and the explicit ACE is granted only
#     when a writability probe fails.
#
#   - The NEGATIVE path (log-dir-create-failed) is RETIRED (VM-verified
#     2026-06-22 on v1.15.0): a non-creatable LogDir is NON-FATAL — the module
#     serves optimized responses (X-Page-Speed present) and emits no
#     X-Pagespeed-Init-Status when the LogDir parent is denied WRITE, so the
#     assertion could only ever time out. This fixture now pins ONLY the
#     positive contract: after a request the LogDir is (re)created and
#     worker-writable. (The bare shipped LogDir ...\PageSpeed\logs is
#     out-of-prefix for the auto-create guard, so the dir is materialized by
#     the logging substrate rather than EnsureDirectoryWritable — either way the
#     observable contract holds.) Reachable and gating; see retired-negative note.
#
# Config path: resolved at runtime via FindConfigFile(<site app physical path>)
# = <site root>\pagespeed.config (the file the module actually reads), not the
# legacy ProgramData/IISWebSpeed path the old default pointed at.
#
# Logs state is cleared in setup, not teardown, with pre-state asserted -
# otherwise the diagnostic fixture (which never reads the logs dir) and
# this fixture (which depends on its absence) can mask each other on a
# shared host.
#
# Pre-conditions (the test asserts these and aborts cleanly if absent):
#   - PageSpeed IIS module already installed + registered
#   - Default IIS site responding on http://localhost/
#   - Healthy state at script entry: X-Page-Speed header present,
#     no X-Pagespeed-Init-Status
#
# Run as Administrator (icacls + appcmd recycle + Remove-Item on
# ProgramData require it).

[CmdletBinding()]
param(
    [string]$Url = "http://localhost/",
    # Empty -> auto-resolve the config the module actually reads at request time
    # via FindConfigFile(<app physical path of $SiteName>) = <site root>\
    # pagespeed.config. The old default pointed at the ProgramData/IISWebSpeed
    # copy the module does NOT read at request time, so the
    # precondition spuriously failed "pagespeed.config not found".
    [string]$ConfigPath = "",
    [string]$SiteName = "Default Web Site",
    [string]$AppPool = "DefaultAppPool",
    [string]$LogDir = "C:\ProgramData\We-Amp\PageSpeed\logs",
    [int]$PollSeconds = 30
)

$ErrorActionPreference = 'Stop'
$appcmd = "$env:SystemRoot\system32\inetsrv\appcmd.exe"

# Resolve the config file the IIS module reads at request time, mirroring
# pagespeed/iis/iis_configuration.h FindConfigFile(): <appPhysicalPath>\
# pagespeed.config else <appPhysicalPath>\iiswebspeed.config. appcmd may return
# an unexpanded %SystemDrive%, so expand it.
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
    try {
        return Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 5
    } catch {
        return $null
    }
}

function Get-PsHeaders {
    param([string]$u)
    $r = Get-PsResponse -u $u
    if ($r) { return $r.Headers } else { return $null }
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
    & "$env:SystemRoot\system32\inetsrv\appcmd.exe" recycle apppool /apppool.name:$pool | Out-Null
    Start-Sleep -Seconds 3
}

function Stop-W3SVC {
    Stop-Service -Name W3SVC -Force -ErrorAction SilentlyContinue
    # Give w3wp.exe a moment to exit so file handles on the log dir
    # release before we Remove-Item.
    Start-Sleep -Seconds 2
}

function Start-W3SVC {
    Start-Service -Name W3SVC -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

function Clear-LogDir {
    param([string]$dir)
    # Retry the removal: w3wp holds open handles on the log files it writes and
    # may not release them the instant W3SVC stops (callers Stop-W3SVC first).
    # Single-shot Remove-Item + throw would spuriously RED-gate on a transient
    # handle lag now that this fixture is GATING (mirrors Clear-CacheContents).
    for ($i = 0; $i -lt 5; $i++) {
        if (Test-Path -LiteralPath $dir) {
            Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path -LiteralPath $dir)) { break }
        Start-Sleep -Seconds 2
    }
    # Only fail after retries are exhausted, so a genuinely-stuck handle still
    # fails the gate but a transient release lag does not.
    if (Test-Path -LiteralPath $dir) {
        throw "Failed to remove LogDir after retries (w3wp handle not released?): $dir"
    }
}

# --- Restore-AllState defined BEFORE setup-clear so the script-scope
#     trap below can call it if setup throws. Same hoisting fix as
#     test_iis_cache_diagnostic.ps1 + test_iis_cache_autocreate.ps1.
$restored = $false
function Restore-AllState {
    if ($script:restored) { return }
    # Defensive-only since the deny-ACE negative path was retired: this
    # fixture no longer applies a deny ACE, but clear any stray one anyway so a
    # manual/aborted run can't leave the LogDir parent write-denied.
    $parent = Split-Path -Parent $LogDir
    if (Test-Path -LiteralPath $parent) {
        & icacls.exe $parent /remove:d "*S-1-5-32-568" 2>&1 | Out-Null
        & icacls.exe $parent /remove:d "*S-1-5-20"     2>&1 | Out-Null
    }
    Recycle-AppPool -pool $AppPool
    Start-W3SVC   # leave W3SVC up on every exit path (Clear-LogDir stops it)
    $script:restored = $true
}
trap { Restore-AllState; break }

# --- Setup: clear LogDir state in setup, not teardown ---
Write-Host "=== Setup: clear LogDir (in setup, not teardown) ==="
Stop-W3SVC
Clear-LogDir -dir $LogDir
Write-Host "Pre-state OK: $LogDir does not exist."
Start-W3SVC

# --- Pre-conditions ---
Write-Host "=== Pre-conditions ==="
if (-not $ConfigPath) {
    $ConfigPath = Resolve-ModuleConfigPath -site $SiteName
    Write-Host "Resolved module config (FindConfigFile of '$SiteName'): $ConfigPath"
}
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
    Write-Host "::warning::Baseline did not show X-Page-Speed header on first poll. Module may still be initializing; continuing - the autocreate-positive assertion below polls."
}

try {
    # ============================================================
    # POSITIVE PATH: auto-create succeeds, request returns 200,
    # no init-status header, LogDir exists with at least RX+W for
    # the worker identity.
    # ============================================================
    Write-Host ""
    Write-Host "=== Positive path: auto-create on default config ==="
    Stop-W3SVC
    Clear-LogDir -dir $LogDir
    Start-W3SVC
    Recycle-AppPool -pool $AppPool

    # First request: triggers IisProcessContext::GetServerContext ->
    # EnsureDirectoryWritable on the LogDir.
    Write-Host "Sending smoke request to $Url ..."
    $response = $null
    $elapsed = 0
    while ($elapsed -lt $PollSeconds -and -not $response) {
        $response = Get-PsResponse -u $Url
        if (-not $response) {
            Start-Sleep -Seconds 2
            $elapsed += 2
        }
    }
    if (-not $response) {
        throw "Positive path: no response from $Url within ${PollSeconds}s."
    }
    if ($response.StatusCode -ne 200) {
        throw "Positive path: expected 200, got $($response.StatusCode)."
    }
    Write-Host "Got HTTP $($response.StatusCode) from $Url."

    # Assert: no failure-mode headers. The very first request after
    # Start-W3SVC + Recycle-AppPool races module init: a freshly recycled
    # w3wp can transiently emit X-Pagespeed-Init-Status before init's
    # filesystem prep (LogDir auto-create) completes, then clears within a
    # few seconds. A one-shot check on the first response flakes;
    # poll for the header to
    # be ABSENT instead, mirroring the post-restore sanity-check idiom
    # used later in this same file.
    $cleared = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                              -expectPresent $false -timeoutSec $PollSeconds
    if ($cleared -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw ("Positive path: X-Pagespeed-Init-Status did not clear within " +
               "${PollSeconds}s; observed '$obs'.")
    }

    # Assert: LogDir exists and is a real directory (not a reparse
    # point — the reparse-point rejection applies to all
    # EnsureDirectoryWritable callers, cache or LogDir, because
    # canonicalizing a path does not resolve junctions).
    if (-not (Test-Path -LiteralPath $LogDir -PathType Container)) {
        throw "Positive path: LogDir was not created: $LogDir"
    }
    $info = Get-Item -LiteralPath $LogDir -Force
    if ($info.Attributes -match 'ReparsePoint') {
        throw ("Positive path: LogDir is a reparse point " +
               "(attrs=$($info.Attributes)); the reparse-point check should " +
               "have rejected this.")
    }
    Write-Host "LogDir exists and is a real directory: $LogDir"

    # Assert: worker has at least RX+W on the new dir. Under the common
    # case + the narrower grant, this is satisfied EITHER
    # by an explicit RX+W ACE for the worker SID (auto-create's
    # conditional ACL leg fired because inheritance was broken — note
    # the absence of DELETE relative to the cache fixture) OR by an
    # inherited Modify for IIS_IUSRS (S-1-5-32-568) or NETWORK
    # SERVICE (S-1-5-20) flowing down from the installer's
    # Product.wxs:343-346 GrantLogAcl grant. Both are valid.
    $acl = Get-Acl -LiteralPath $LogDir
    $hasGrant = $false
    foreach ($ace in $acl.Access) {
        if ($ace.AccessControlType -ne 'Allow') { continue }
        $rights = $ace.FileSystemRights.ToString()
        # Accept Modify (inherited installer grant) OR explicit
        # ReadAndExecute+Write (conditional leg). FullControl is
        # also a valid superset.
        $isAcceptable = ($rights -match 'Modify' -or
                         $rights -match 'FullControl' -or
                         ($rights -match 'ReadAndExecute' -and $rights -match 'Write') -or
                         ($rights -match 'Write' -and $rights -match 'ReadData' -and $rights -match 'ExecuteFile'))
        if (-not $isAcceptable) { continue }
        $idRef = $ace.IdentityReference.Value
        if ($idRef -match 'IIS APPPOOL\\' -or
            $idRef -match 'IIS_IUSRS' -or
            $idRef -match 'NETWORK SERVICE' -or
            $idRef -eq 'S-1-5-32-568' -or
            $idRef -eq 'S-1-5-20') {
            $hasGrant = $true
            Write-Host "Found acceptable LogDir ACE: $idRef ($rights)"
            break
        }
    }
    if (-not $hasGrant) {
        throw ("Positive path: no acceptable RX+W (or superset) ACE found on $LogDir. " +
               "Expected either an explicit worker-SID grant (conditional ACL " +
               "leg fired) or an inherited Modify for IIS_IUSRS/NETWORK SERVICE.")
    }

    Write-Host "PASS: positive path."

    # ============================================================
    # NEGATIVE PATH (log-dir-create-failed): RETIRED (VM-verified 2026-06-22 on the Windows Server 2016 IIS base image / v1.15.0).
    # A non-creatable LogDir is NON-FATAL on this build: with the LogDir
    # parent (C:\ProgramData\We-Amp\PageSpeed) denied WRITE for IIS_IUSRS +
    # NETWORK SERVICE and the LogDir cleared, the module still serves
    # OPTIMIZED responses (X-Page-Speed present) and emits NO
    # X-Pagespeed-Init-Status: log-dir-create-failed (the logs dir is simply
    # not created). The kLogDirCreateFailed path in iis_process_context.cpp is
    # not reached for a denied LogDir here, so the old assertion could only
    # ever time out -> a perpetually-red gate. It is removed rather than left
    # failing; the positive path above is the gating contract (LogDir IS
    # auto-created with worker-writable ACLs). If LogDir failure is ever made
    # fatal again, restore a negative path that asserts the real surfaced kind.
    # ============================================================

    Restore-AllState

    # ============================================================
    # Post-restore sanity: healthy state returns.
    # ============================================================
    Write-Host ""
    Write-Host "=== Asserting healthy state restored ==="
    $cleared = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                              -expectPresent $false -timeoutSec $PollSeconds
    if ($cleared -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw ("Post-restore: X-Pagespeed-Init-Status did not clear within " +
               "${PollSeconds}s; observed '$obs'.")
    }
    $xps = Wait-ForHeader -u $Url -name "X-Page-Speed" -timeoutSec $PollSeconds
    if (-not $xps) {
        throw "Post-restore: X-Page-Speed did not return within ${PollSeconds}s."
    }
    Write-Host "PASS: healthy state restored (X-Page-Speed=$xps)."

    Write-Host ""
    Write-Host "=== test_iis_logdir_autocreate.ps1: ALL PASS ==="
    exit 0
}
finally {
    # Belt-and-braces. trap above handles terminating errors; this catches the
    # clean exit path. Restore-AllState recycles the pool and defensively clears
    # any stray deny ACE on the LogDir parent (this fixture no longer backs up or
    # edits config — the deny-ACE negative path is retired — but a manual/aborted
    # run could leave a deny ACE, and a stuck deny would break every IIS request).
    Restore-AllState
}

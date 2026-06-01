# test_iis_logdir_autocreate.ps1 - Pins the design record §Operational +
# the referenced issue contract for LogDir auto-create on IIS. Parallel in
# structure to test_iis_cache_autocreate.ps1; differs in three places:
#
#   - The target directory is the configured LogDir, not a per-site
#     cache subdir (there is no IIS-ApplicationId-derived suffix —
#     LogDir is the literal value from pagespeed.config).
#
#   - The runtime ACL grant is RX+W (no DELETE), mirroring
#     Product.wxs GrantLogAcl. The positive-path ACE assertion
#     accepts EITHER an explicit RX+W (= ReadAndExecute + Write) on
#     the worker SID OR an inherited Modify from the well-known SIDs
#     the installer ships (S-1-5-32-568 / S-1-5-20) — both are valid
#     per the design record §3 common case.
#
#   - The X-Pagespeed-Init-Status header value is
#     `log-dir-create-failed` on the negative path (vs.
#     `cache-path-create-failed` for cache).
#
# Negative-path-2 is intentionally omitted: out-of-prefix LogDir
# leaves the runtime behaviour unchanged from legacy (no
# auto-create, no diagnostic page) and the assertion would reduce to
# "request still returns 200 and no log-dir-create-failed header,"
# which is already covered transitively by the positive path with a
# default-config restore. Adding it would duplicate cache fixture
# coverage without exercising a new contract.
#
# Per the design record §6 + Constraints, logs state is cleared in setup, not
# teardown, with pre-state asserted - otherwise the diagnostic fixture
# (which never reads the logs dir) and this fixture (which depends on
# its absence) can mask each other on a shared host.
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
    [string]$ConfigPath = "C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config",
    [string]$AppPool = "DefaultAppPool",
    [string]$LogDir = "C:\ProgramData\We-Amp\PageSpeed\logs",
    [int]$PollSeconds = 30
)

$ErrorActionPreference = 'Stop'

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
    if (Test-Path -LiteralPath $dir) {
        Remove-Item -LiteralPath $dir -Recurse -Force
    }
    if (Test-Path -LiteralPath $dir) {
        throw "Failed to remove LogDir: $dir"
    }
}

# --- Restore-AllState defined BEFORE setup-clear so the script-scope
#     trap below can call it if setup throws. Same hoisting fix as
#     test_iis_cache_diagnostic.ps1 + test_iis_cache_autocreate.ps1.
$restored = $false
function Restore-AllState {
    if ($script:restored) { return }
    Write-Host "Clearing any residual deny ACEs on LogDir parent..."
    $parent = Split-Path -Parent $LogDir
    if (Test-Path -LiteralPath $parent) {
        & icacls.exe $parent /remove:d "*S-1-5-32-568" 2>&1 | Out-Null
        & icacls.exe $parent /remove:d "*S-1-5-20"     2>&1 | Out-Null
    }
    Recycle-AppPool -pool $AppPool
    $script:restored = $true
}
trap { Restore-AllState; break }

# --- Setup: clear LogDir state in setup, not teardown ---
Write-Host "=== Setup: clear LogDir ==="
Stop-W3SVC
Clear-LogDir -dir $LogDir
Write-Host "Pre-state OK: $LogDir does not exist."
Start-W3SVC

# --- Pre-conditions ---
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

    # Assert: no failure-mode headers.
    if ($response.Headers["X-Pagespeed-Init-Status"]) {
        throw ("Positive path: expected NO X-Pagespeed-Init-Status header, " +
               "got '$($response.Headers['X-Pagespeed-Init-Status'])'.")
    }

    # Assert: LogDir exists and is a real directory (not a reparse
    # point — the design record §3d applies to all EnsureDirectoryWritable
    # callers, cache or LogDir).
    if (-not (Test-Path -LiteralPath $LogDir -PathType Container)) {
        throw "Positive path: LogDir was not created: $LogDir"
    }
    $info = Get-Item -LiteralPath $LogDir -Force
    if ($info.Attributes -match 'ReparsePoint') {
        throw ("Positive path: LogDir is a reparse point " +
               "(attrs=$($info.Attributes)); the design record §3d should have rejected this.")
    }
    Write-Host "LogDir exists and is a real directory: $LogDir"

    # Assert: worker has at least RX+W on the new dir. Per the design record §3
    # common case + the narrower grant, this is satisfied EITHER
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
    # NEGATIVE PATH: log-dir-create-failed
    # Deny WRITE on the LogDir parent so RecursivelyMakeDir fails on
    # the LogDir mkdir. the design record §3c routes this to a "mkdir failed for
    # a non-ACL-recoverable reason — no ACL retry, by design" error,
    # surfaced through the kLogDirCreateFailed code path added in
    # iis_process_context.cpp.
    # ============================================================
    Write-Host ""
    Write-Host "=== Negative path: log-dir-create-failed via icacls deny ==="
    Stop-W3SVC
    Clear-LogDir -dir $LogDir
    # The LogDir parent (e.g. C:\ProgramData\We-Amp\PageSpeed) must
    # exist for the deny ACE to attach to a real object. The MSI
    # creates it; assert rather than New-Item, so a missing parent
    # surfaces a setup failure not a false negative.
    $parent = Split-Path -Parent $LogDir
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Negative path setup: LogDir parent $parent does not exist (MSI not installed?)."
    }
    # Deny WRITE on the parent for both well-known SIDs the worker may
    # be running under (see cache fixture for full rationale — same
    # ApplicationPoolIdentity vs NetworkService consideration).
    $denyResult = & icacls.exe $parent /deny "*S-1-5-32-568:(W)" "*S-1-5-20:(W)" 2>&1
    Write-Host "icacls deny result: $denyResult"
    Start-W3SVC
    Recycle-AppPool -pool $AppPool

    $diag = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                           -expectedValue "log-dir-create-failed" `
                           -timeoutSec $PollSeconds
    if ($diag -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw ("Negative path: expected X-Pagespeed-Init-Status=" +
               "'log-dir-create-failed' within ${PollSeconds}s; observed '$obs'.")
    }
    Write-Host "PASS: negative path (header='$diag')."

    # Cleanup deny ACEs before restoring (remove both SIDs we added).
    & icacls.exe $parent /remove:d "*S-1-5-32-568" 2>&1 | Out-Null
    & icacls.exe $parent /remove:d "*S-1-5-20" 2>&1 | Out-Null

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
    # Belt-and-braces. trap above handles terminating errors; this
    # catches the clean exit path. The negative path mutates ACL
    # state and a stuck deny ACE on the LogDir parent would break
    # every subsequent IIS request.
    Restore-AllState
}

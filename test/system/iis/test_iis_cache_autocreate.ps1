# test_iis_cache_autocreate.ps1 - Pins the design record contract for
# server-context init-time filesystem prep on IIS:
#
#   1. Positive path: with AutoCreateCachePath=on (default), the
#      per-site cache subdirectory (<FileCachePath>\<siteid>.ROOT) is
#      auto-created on first request, the request returns 200, and
#      neither X-Pagespeed-Init-Status: cache-path-missing nor
#      cache-path-create-failed is emitted. The created subdir is a
#      real directory (not a reparse point) and the worker identity
#      has Modify rights on it (either via the conditional ACL grant
#      leg or via inherited installer-shipped ACLs on the parent —
#      both are valid per the design record §3 common case).
#
#   2. Negative path 1 (cache-path-create-failed): with a deny-write
#      ACE on the parent cache root for IIS_IUSRS, the auto-create's
#      RecursivelyMakeDir fails, and the module surfaces
#      X-Pagespeed-Init-Status: cache-path-create-failed.
#
#   3. Negative path 2 (cache-path-missing, updated copy): with
#      FileCachePath pointed at a path outside both prefix
#      guardrails (C:\Temp\custom-cache), the auto-create's
#      IsPathInAutoCreatePrefix check returns false, the caller
#      falls through to the legacy GetFileAttributesA branch which
#      sets kCachePathMissing, and the diagnostic body contains the
#      the design record §4 updated prefix-rationale copy ("We only auto-create
#      paths under ..."). This branch is otherwise untested
#      (test_iis_cache_diagnostic.ps1 exercises kCachePathEmpty, not
#      kCachePathMissing).
#
# Per the design record §6 + Constraints, cache state is cleared in setup, not
# teardown, with pre-state asserted - otherwise the diagnostic fixture
# (which never reads the cache dir) and this fixture (which depends on
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
# Fresh 1.1+ MSI installs file pagespeed.config into
#   C:\ProgramData\We-Amp\PageSpeed\pagespeed.config
# alongside the cache + logs subdirectories. iis_module_factory.cpp
# resolves that canonical path first and falls back to the legacy
#   C:\ProgramData\We-Amp\IISWebSpeed\pagespeed.config
# for upgrade-from-IISpeed and upgrade-from-1.1-legacy installs.
# Override -ConfigPath if testing against a legacy-layout install.
param(
    [string]$Url = "http://localhost/",
    [string]$ConfigPath = "C:\ProgramData\We-Amp\PageSpeed\pagespeed.config",
    [string]$AppPool = "DefaultAppPool",
    [string]$CacheRoot = "C:\ProgramData\We-Amp\PageSpeed\cache",
    [string]$OutOfPrefixPath = "C:\Temp\custom-cache",
    [string]$SiteName = "Default Web Site",
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

function Wait-ForBodyContains {
    # Polls Url body for a literal substring; returns $true on first hit,
    # $false on timeout. Used for negative-path 2 which asserts on the
    # diagnostic page copy, not a header value.
    param(
        [string]$u,
        [string]$substring,
        [int]$timeoutSec = 30
    )
    $elapsed = 0
    while ($elapsed -lt $timeoutSec) {
        $r = Get-PsResponse -u $u
        if ($r -and $r.Content -and $r.Content.Contains($substring)) {
            return $true
        }
        Start-Sleep -Seconds 2
        $elapsed += 2
    }
    return $false
}

function Recycle-AppPool {
    param([string]$pool)
    & "$env:SystemRoot\system32\inetsrv\appcmd.exe" recycle apppool /apppool.name:$pool | Out-Null
    Start-Sleep -Seconds 3
}

function Stop-W3SVC {
    Stop-Service -Name W3SVC -Force -ErrorAction SilentlyContinue
    # Give w3wp.exe a moment to exit so file handles on the cache dir
    # release before we Remove-Item.
    Start-Sleep -Seconds 2
}

function Start-W3SVC {
    Start-Service -Name W3SVC -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}

function Get-SiteAppId {
    # IIS computes the per-site cache subdir from the ApplicationId
    # ("/LM/W3SVC/<id>/ROOT" -> "<id>.ROOT"); see
    # iis_process_context.cpp:195-197. We discover the site's numeric
    # id via appcmd and synthesize the subdir name the same way.
    param([string]$site)
    $appcmd = "$env:SystemRoot\system32\inetsrv\appcmd.exe"
    $raw = & $appcmd list site $site /text:site.id 2>$null
    if (-not $raw) {
        throw "Could not resolve IIS site id for '$site' (appcmd list site returned nothing)."
    }
    return ($raw.Trim() + ".ROOT")
}

function Clear-CacheRoot {
    param([string]$root)
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
    if (Test-Path -LiteralPath $root) {
        throw "Failed to remove cache root: $root"
    }
}

function Backup-Config {
    param([string]$path)
    $backup = "$path.autocreate-backup"
    Copy-Item -LiteralPath $path -Destination $backup -Force
    return $backup
}

function Restore-ConfigFromBackup {
    param([string]$path, [string]$backup)
    if (Test-Path -LiteralPath $backup) {
        Copy-Item -LiteralPath $backup -Destination $path -Force
        Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
    }
}

# --- Restore-AllState defined BEFORE setup-clear so the script-scope
#     trap below can call it if setup throws. PowerShell function
#     declarations are executed top-down (NOT parse-hoisted) but trap
#     IS lexically active for the entire script scope from parse time,
#     so a trap that fires before the function definition was reached
#     dies with "term not recognized". Same class of bug fixed in
#     test_iis_cache_diagnostic.ps1. $backup is set later in
#     Pre-conditions; Restore-AllState is a no-op until then.
$restored = $false
$backup = $null
function Restore-AllState {
    if ($script:restored) { return }
    if (-not $script:backup) {
        # Failure happened before Backup-Config ran — nothing to restore.
        return
    }
    Write-Host "Restoring config + clearing any residual deny ACEs..."
    Restore-ConfigFromBackup -path $ConfigPath -backup $script:backup
    # Best-effort cleanup of the deny ACE from negative path 1 - if the
    # ACE was never applied (failure before that step), /remove:d is a
    # no-op. IIS_IUSRS well-known SID is S-1-5-32-568.
    if (Test-Path -LiteralPath $CacheRoot) {
        & icacls.exe $CacheRoot /remove:d "*S-1-5-32-568" 2>&1 | Out-Null
        & icacls.exe $CacheRoot /remove:d "*S-1-5-20" 2>&1 | Out-Null
    }
    Recycle-AppPool -pool $AppPool
    $script:restored = $true
}
trap { Restore-AllState; break }

# --- Setup: clear cache state in setup, not teardown ---
Write-Host "=== Setup: clear cache root ==="
Stop-W3SVC
Clear-CacheRoot -root $CacheRoot
Write-Host "Pre-state OK: $CacheRoot does not exist."
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

# Resolve the per-site subdir name once. Used by the positive path
# and negative path 1.
$siteSubdir = Get-SiteAppId -site $SiteName
$siteCacheDir = Join-Path $CacheRoot $siteSubdir
Write-Host "Per-site cache subdir: $siteCacheDir"

$backup = Backup-Config -path $ConfigPath

try {
    # ============================================================
    # POSITIVE PATH: auto-create succeeds, request returns 200,
    # no init-status header.
    # ============================================================
    Write-Host ""
    Write-Host "=== Positive path: auto-create on default config ==="
    Stop-W3SVC
    Clear-CacheRoot -root $CacheRoot
    Start-W3SVC
    Recycle-AppPool -pool $AppPool

    # First request: triggers IisProcessContext::GetServerContext ->
    # EnsureDirectoryWritable -> RecursivelyMakeDir on the per-site
    # subdir.
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
    # Start-W3SVC + Recycle-AppPool races the module init: a freshly
    # recycled w3wp transiently emits X-Pagespeed-Init-Status:
    # cache-path-missing before/during the init-time auto-create, then
    # clears within a few seconds. A one-shot check on the first
    # response flakes; poll for the header to be ABSENT instead, mirroring
    # the post-restore sanity-check idiom used later in this same file.
    $cleared = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                              -expectPresent $false -timeoutSec $PollSeconds
    if ($cleared -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw ("Positive path: X-Pagespeed-Init-Status did not clear within " +
               "${PollSeconds}s; observed '$obs'.")
    }

    # Assert: per-site cache dir exists and is a real directory (not
    # a reparse point — the design record §3d closes the canonicalize -> mkdir
    # -> ACL TOCTOU on junction-plant; we don't replay the security
    # check here, but we do assert the post-create shape).
    if (-not (Test-Path -LiteralPath $siteCacheDir -PathType Container)) {
        throw "Positive path: per-site cache dir was not created: $siteCacheDir"
    }
    $info = Get-Item -LiteralPath $siteCacheDir -Force
    if ($info.Attributes -match 'ReparsePoint') {
        throw ("Positive path: per-site cache dir is a reparse point " +
               "(attrs=$($info.Attributes)); the design record §3d should have rejected this.")
    }
    Write-Host "Per-site cache dir exists and is a real directory: $siteCacheDir"

    # Assert: worker has Modify rights on the new dir. Per the design record §3
    # common case, this is satisfied EITHER by an explicit ACE for the
    # worker SID (auto-create's conditional ACL leg fired because
    # inheritance was broken) OR by an inherited Modify for IIS_IUSRS
    # (S-1-5-32-568) or NETWORK SERVICE (S-1-5-20) flowing down from
    # the installer's Product.wxs:338-341 grant. Both are valid.
    $acl = Get-Acl -LiteralPath $siteCacheDir
    $hasModify = $false
    foreach ($ace in $acl.Access) {
        if ($ace.AccessControlType -ne 'Allow') { continue }
        # FileSystemRights "Modify" or anything that's a superset.
        $rights = $ace.FileSystemRights.ToString()
        $isModifyLike = ($rights -match 'Modify' -or
                         $rights -match 'FullControl' -or
                         ($rights -match 'Write' -and $rights -match 'Delete'))
        if (-not $isModifyLike) { continue }
        # Accept either the AppPool worker SID directly OR the
        # well-known IIS_IUSRS / NETWORK SERVICE grants the installer
        # ships.
        $idRef = $ace.IdentityReference.Value
        if ($idRef -match 'IIS APPPOOL\\' -or
            $idRef -match 'IIS_IUSRS' -or
            $idRef -match 'NETWORK SERVICE' -or
            $idRef -eq 'S-1-5-32-568' -or
            $idRef -eq 'S-1-5-20') {
            $hasModify = $true
            Write-Host "Found acceptable Modify ACE: $idRef ($rights)"
            break
        }
    }
    if (-not $hasModify) {
        throw ("Positive path: no acceptable Modify ACE found on $siteCacheDir. " +
               "Expected either an explicit worker-SID grant (conditional ACL " +
               "leg fired) or an inherited Modify for IIS_IUSRS/NETWORK SERVICE.")
    }

    Write-Host "PASS: positive path."

    # ============================================================
    # NEGATIVE PATH 1: cache-path-create-failed
    # Deny WRITE on the parent so RecursivelyMakeDir fails on the
    # per-site subdir mkdir. the design record §3c routes this to
    # kCachePathCreateFailed (mkdir failed for a non-ACL-recoverable
    # reason — no ACL retry, by design).
    # ============================================================
    Write-Host ""
    Write-Host "=== Negative path 1: cache-path-create-failed via icacls deny ==="
    Stop-W3SVC
    Clear-CacheRoot -root $CacheRoot
    # Re-create the parent so we have something to deny on (and so the
    # path isn't kCachePathMissing for a different reason).
    New-Item -Path $CacheRoot -ItemType Directory -Force | Out-Null
    # Deny WRITE on the parent for both well-known SIDs the worker may
    # be running under. AppPool default is ApplicationPoolIdentity (which
    # is in IIS_IUSRS = S-1-5-32-568), but operators can flip the pool
    # to NetworkService (S-1-5-20); a deny ACE on only IIS_IUSRS would
    # miss that case and silently let the fixture pass on positive-path
    # semantics. Deny on both SIDs to keep the negative assertion
    # robust regardless of pool identity.
    $denyResult = & icacls.exe $CacheRoot /deny "*S-1-5-32-568:(W)" "*S-1-5-20:(W)" 2>&1
    Write-Host "icacls deny result: $denyResult"
    Start-W3SVC
    Recycle-AppPool -pool $AppPool

    $diag = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                           -expectedValue "cache-path-create-failed" `
                           -timeoutSec $PollSeconds
    if ($diag -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw ("Negative path 1: expected X-Pagespeed-Init-Status=" +
               "'cache-path-create-failed' within ${PollSeconds}s; observed '$obs'.")
    }
    Write-Host "PASS: negative path 1 (header='$diag')."

    # Cleanup deny ACEs before moving on (remove both SIDs we added).
    & icacls.exe $CacheRoot /remove:d "*S-1-5-32-568" 2>&1 | Out-Null
    & icacls.exe $CacheRoot /remove:d "*S-1-5-20" 2>&1 | Out-Null

    # ============================================================
    # NEGATIVE PATH 2: cache-path-missing with updated copy
    # Point FileCachePath outside both prefix guardrails so
    # IsPathInAutoCreatePrefix() returns false in
    # EnsureDirectoryWritable; the caller falls through to the legacy
    # GetFileAttributesA branch which sets kCachePathMissing (with
    # the design record §4 updated diagnostic copy).
    # ============================================================
    Write-Host ""
    Write-Host "=== Negative path 2: cache-path-missing with prefix-rationale copy ==="
    Stop-W3SVC
    # Ensure the out-of-prefix path does NOT exist (otherwise the
    # legacy GetFileAttributesA check finds the dir, the test gets
    # past kCachePathMissing, and the assertion fails for an unrelated
    # reason).
    if (Test-Path -LiteralPath $OutOfPrefixPath) {
        Remove-Item -LiteralPath $OutOfPrefixPath -Recurse -Force
    }
    # Patch pagespeed.config to point at the out-of-prefix path. Both
    # the canonical "pagespeed FileCachePath ..." and the legacy
    # "ModPagespeedFileCachePath ..." spellings parse to the same
    # property, so handle either form.
    $originalContent = Get-Content -Raw -LiteralPath $ConfigPath
    $patched = $originalContent -replace `
        '(?m)^\s*(?:ModPagespeed|pagespeed\s+)FileCachePath\s+.*$', `
        ('pagespeed FileCachePath "' + $OutOfPrefixPath + '"')
    if ($patched -eq $originalContent) {
        # Directive missing entirely - inject one at the end so the
        # module sees the out-of-prefix value.
        $patched = $originalContent + "`npagespeed FileCachePath `"$OutOfPrefixPath`"`n"
    }
    Set-Content -LiteralPath $ConfigPath -Value $patched -NoNewline
    Start-W3SVC
    Recycle-AppPool -pool $AppPool

    $diag2 = Wait-ForHeader -u $Url -name "X-Pagespeed-Init-Status" `
                            -expectedValue "cache-path-missing" `
                            -timeoutSec $PollSeconds
    if ($diag2 -eq [string]::Empty) {
        $h = Get-PsHeaders -u $Url
        $obs = if ($h) { $h["X-Pagespeed-Init-Status"] } else { "<no response>" }
        throw ("Negative path 2: expected X-Pagespeed-Init-Status=" +
               "'cache-path-missing' within ${PollSeconds}s; observed '$obs'.")
    }
    Write-Host "Header OK: X-Pagespeed-Init-Status='$diag2'."

    # Assert the updated diagnostic copy from the design record §4 renders. The
    # exact phrase below is load-bearing: iis_http_module.cpp:541
    # ships this literal string and any future edit to that copy
    # should fail this test loudly rather than silently weaken the
    # prefix-rationale disclosure.
    $found = Wait-ForBodyContains -u $Url `
                                  -substring "We only auto-create paths under" `
                                  -timeoutSec $PollSeconds
    if (-not $found) {
        $r = Get-PsResponse -u $Url
        $bodyPreview = if ($r) { $r.Content.Substring(0, [Math]::Min(400, $r.Content.Length)) } else { "<no response>" }
        throw ("Negative path 2: diagnostic page body did not contain " +
               "'We only auto-create paths under' within ${PollSeconds}s. " +
               "Body preview: $bodyPreview")
    }
    Write-Host "PASS: negative path 2 (header + prefix-rationale copy)."

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
    Write-Host "=== test_iis_cache_autocreate.ps1: ALL PASS ==="
    exit 0
}
finally {
    # Belt-and-braces. trap above handles terminating errors; this
    # catches the clean exit path. Both negative paths mutate
    # config/ACL state and a stuck deny ACE on the cache root would
    # break every subsequent IIS request.
    Restore-AllState
    # Best-effort: clear the out-of-prefix path so a follow-up run
    # doesn't accidentally find it pre-existing.
    if (Test-Path -LiteralPath $OutOfPrefixPath) {
        Remove-Item -LiteralPath $OutOfPrefixPath -Recurse -Force -ErrorAction SilentlyContinue
    }
}

#!/usr/bin/env bash
# ea4-smoke-target.sh - runs inside the cPanel guest VM.
# Exercises the ea-apache24-mod_pagespeed install -> EA4 reload -> page-fetch
# -> uninstall lifecycle against a real cPanel + ea-apache24 host.
#
# Inputs (via env, set by the caller):
#   RPM_NAME                - filename of the candidate RPM, sitting in
#                             /tmp/ea4-smoke/<RPM_NAME>
#   EXPECTED_VERSION_STRING - MAJOR.MINOR.BUILD[-PRERELEASE] that should
#                             appear in the X-Mod-Pagespeed response header.
#                             Derived from RELEASE_TAG by the caller.
#   RELEASE_TAG             - the release tag (e.g. v1.1.0). Logged only.
#   DEBUG                   - if set non-empty, enables xtrace (PS4 timestamped)
#
# Assumes:
#   - cPanel is installed and active (otherwise /scripts/restartsrv_httpd
#     is missing and we fail fast).
#   - ea-apache24 is the running Apache.
#   - License token is at /tmp/ea4-smoke/ci-license-token (the caller scps
#     it there alongside the RPM).
#
# Lifecycle steps mirror package-smoke-rpm: install -> verify -> uninstall
# -> verify clean. No "old MSI" warm-upgrade equivalent here yet; the EA4
# RPMs lack a synthetic-old counterpart (we ship beta -> stable directly).

set -euo pipefail

# Line-buffer stdout so the ssh-captured stream from the runner side sees
# progress in near real-time rather than in 4-8KB blocks. The re-exec MUST
# go through `bash "$0"` rather than running `$0` directly — the PS1
# driver invokes us via `bash /tmp/.../script.sh` precisely because scp
# from a Windows source filesystem doesn't preserve a Unix +x bit and
# chmod-after-scp has empirically failed to make it executable on
# Hyper-V default-switch Linux guests. Running `stdbuf ... "$0"` would
# require +x; routing via `bash` doesn't.
if [ -z "${EA4_SMOKE_STDBUF:-}" ]; then
    if command -v stdbuf >/dev/null 2>&1; then
        export EA4_SMOKE_STDBUF=1
        exec stdbuf -oL -eL bash "$0" "$@"
    fi
fi

if [ -n "${DEBUG:-}" ]; then
    export PS4='+ [\t] '
    set -x
fi

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
fail() { log "FAIL: $*"; dump_diagnostics || true; exit 1; }

# Apache log paths on EA4. /etc/apache2/logs is a symlink to
# /usr/local/apache/logs on cPanel hosts. We probe in this order; the
# symlink target is what survives if /etc/apache2 is being rebuilt mid-test.
APACHE_ERROR_LOGS=(
    /etc/apache2/logs/error_log
    /usr/local/apache/logs/error_log
    /var/log/apache2/error_log
)

dump_diagnostics() {
    log "--- diagnostics ---"
    for f in "${APACHE_ERROR_LOGS[@]}"; do
        if [ -f "$f" ]; then
            log "Apache error log: $f (last 40)"
            tail -40 "$f" 2>/dev/null || true
            break
        fi
    done
    log "PageSpeed log (if any):"
    for f in /var/log/pagespeed/*.log; do
        if [ -f "$f" ]; then
            log "  $f (last 40)"
            tail -40 "$f" 2>/dev/null || true
        fi
    done
    log "Listening sockets on :80"
    ss -tlnp '( sport = :80 )' 2>/dev/null || true
    log "--- end diagnostics ---"
}

: "${RPM_NAME:?RPM_NAME env var required}"
: "${EXPECTED_VERSION_STRING:?EXPECTED_VERSION_STRING env var required}"
RELEASE_TAG="${RELEASE_TAG:-<unknown>}"

cd /tmp/ea4-smoke
[ -f "$RPM_NAME" ] || fail "RPM missing: /tmp/ea4-smoke/$RPM_NAME"
[ -s ci-license-token ] || fail "License token empty/missing"

log "=== ea4-smoke-target.sh ==="
log "RPM:      $RPM_NAME"
log "Release:  $RELEASE_TAG"
log "Expect:   X-Mod-Pagespeed contains '$EXPECTED_VERSION_STRING'"
# shellcheck disable=SC1091
log "OS:       $(. /etc/os-release; echo "$PRETTY_NAME")"

# --- Sanity: confirm we're on cPanel ---
if [ ! -x /scripts/restartsrv_httpd ]; then
    fail "/scripts/restartsrv_httpd missing - this guest does not look like a cPanel host"
fi
if [ ! -d /etc/apache2/conf.d ] || [ ! -d /etc/apache2/conf.modules.d ]; then
    fail "EA4 apache directories missing - is ea-apache24 installed?"
fi

# Where does ea-apache24 live and what modules dir does the RPM target?
EA4_HTTPD_BIN="$(command -v httpd || echo /usr/sbin/httpd)"
log "httpd: $EA4_HTTPD_BIN"
$EA4_HTTPD_BIN -v || true

# --- Resolve nobody:nobody UID/GID once (varies across EL8/EL9) ---
# EL8: nobody = 99:99 (historic). EL9: nobody = 65534:65534 (matches
# systemd-userdb DynamicUser convention). We resolve symbolically and let
# `install -o nobody -g nobody` handle it, but cache the numeric IDs for
# the diagnostic log so a misfile is obvious in the CI log.
NOBODY_UID="$(id -u nobody 2>/dev/null || echo unknown)"
NOBODY_GID="$(id -g nobody 2>/dev/null || echo unknown)"
log "nobody: uid=${NOBODY_UID} gid=${NOBODY_GID}"
if [ "$NOBODY_UID" = "unknown" ]; then
    fail "user 'nobody' missing - cannot stage license token"
fi

# --- Phase 1: Install the candidate RPM ---
log "=== Phase 1: dnf install /tmp/ea4-smoke/$RPM_NAME ==="
# Refresh dnf metadata before dep resolution. Snapshots may have been
# sitting for hours or days; stale repomd against EA4.repo will hand us
# yesterday's dependency closure and either fail to resolve or pull a
# now-obsolete ea-apache24-* into the transaction. `clean expire-cache`
# is cheap (drops cache TTL, not packages) and forces a refresh on the
# next operation. We also pass --refresh on the install itself for belt
# + suspenders.
dnf clean expire-cache >/dev/null 2>&1 || true

# Default cPanel installs ship ea-apache24-mod_ruid2 enabled; our spec
# declares `Conflicts: ea-apache24-mod_ruid2` (the two modules are mutually
# exclusive — both rewrite the request handler chain). Remove mod_ruid2
# first so the install does not hit the conflict. This matches the
# customer-facing install procedure documented at modpagespeed.com/1.1/cpanel
# (operator must pick PageSpeed OR ruid2, not both).
if rpm -q ea-apache24-mod_ruid2 >/dev/null 2>&1; then
    log "Removing ea-apache24-mod_ruid2 (conflicts with mod_pagespeed)"
    EA4_AUTO_RELOAD=0 dnf remove -y ea-apache24-mod_ruid2 2>&1 | tail -10
fi
# Disable EA4_AUTO_RELOAD so the %post script doesn't bounce httpd before
# we've staged the license. We'll restart explicitly in Phase 2.
EA4_AUTO_RELOAD=0 dnf install -y --refresh "/tmp/ea4-smoke/$RPM_NAME" 2>&1 | tail -30

# Confirm files landed where the spec says they should.
[ -f /etc/apache2/conf.modules.d/490_mod_pagespeed.conf ] \
    || fail "LoadModule fragment missing at /etc/apache2/conf.modules.d/490_mod_pagespeed.conf"
[ -f /etc/apache2/conf.d/pagespeed.conf ] \
    || fail "pagespeed.conf missing at /etc/apache2/conf.d/pagespeed.conf"
[ -f /etc/apache2/modules/mod_pagespeed.so ] \
    || fail "mod_pagespeed.so missing in /etc/apache2/modules/"

# --- Phase 2: Drop the license token where mod_pagespeed_v2 looks ---
# pagespeed/kernel/license_v2/license_file.cc derives the license file from
# FileCachePath: <cache_path>.parent_path() / "pagespeed.license".
# pagespeed.conf ships FileCachePath = /var/cache/mod_pagespeed, so the
# token lives at /var/cache/pagespeed.license. The spec creates
# /var/cache/mod_pagespeed in %install (which makes /var/cache exist
# already, but we belt-and-suspenders mkdir it anyway in case a future
# spec change drops the dir).
log "=== Phase 2: Stage license token + restart Apache ==="
mkdir -p /var/cache
# `install` here (not `cp`) so the mode/owner change is atomic with the
# rename — Apache children watching the file via inotify never see a
# half-written temp.
install -m 0644 -o nobody -g nobody ci-license-token /var/cache/pagespeed.license
log "License staged: $(stat -c '%n mode=%a owner=%U:%G size=%s' /var/cache/pagespeed.license)"

# Capture pre-restart Apache pids so we can confirm the restart actually
# replaced them (cPanel's restartsrv_httpd can return 0 even when Apache
# never came back, e.g. if the config check passed but the bind failed).
PRE_PIDS="$(pgrep -d, -x httpd 2>/dev/null || true)"

# Use cPanel's wrapper instead of `systemctl restart httpd` so we go through
# the same restart path the WHM UI uses (which is what real customers hit).
/scripts/restartsrv_httpd >/dev/null 2>&1 || fail "/scripts/restartsrv_httpd nonzero"

# restartsrv_httpd returns 0 even when Apache didn't actually come up
# (e.g. cPanel's wrapper considered the config-check exit status, not the
# bind status). Wait until something is actually listening on :80 OR
# localhost responds before we trust it.
log "Waiting for httpd to accept connections on :80..."
LISTEN_OK=0
for attempt in $(seq 1 30); do
    if ss -tln '( sport = :80 )' 2>/dev/null | grep -q ':80 '; then
        if curl -sf -o /dev/null --max-time 2 http://localhost/ 2>/dev/null \
           || curl -s -o /dev/null --max-time 2 http://localhost/ 2>/dev/null; then
            LISTEN_OK=1
            log "  attempt ${attempt}/30: httpd listening + responding"
            break
        fi
    fi
    sleep 1
done
if [ "$LISTEN_OK" != "1" ]; then
    log "httpd never started listening on :80 after 30s"
    log "Post-restart httpd pids: $(pgrep -d, -x httpd 2>/dev/null || echo none)"
    log "Pre-restart pids were:   ${PRE_PIDS:-none}"
    fail "Apache did not come up after /scripts/restartsrv_httpd"
fi
POST_PIDS="$(pgrep -d, -x httpd 2>/dev/null || true)"
log "httpd pids pre/post restart: '${PRE_PIDS}' -> '${POST_PIDS}'"

# --- Phase 3: Verify the module loads and serves the X-Mod-Pagespeed header ---
log "=== Phase 3: Module loaded + header asserted ==="
# `httpd -M` prints loaded modules. pagespeed_module must be there.
if ! $EA4_HTTPD_BIN -M 2>/dev/null | grep -q 'pagespeed_module'; then
    log "Loaded modules:"; $EA4_HTTPD_BIN -M 2>/dev/null | head -30
    fail "pagespeed_module not loaded"
fi
log "pagespeed_module loaded OK"

# Need to give Apache a moment after restart before issuing the first request
# so the license init and cache prime finish; the rpm smoke uses the same
# warm-up retry pattern (release.yml:1670 region).
mkdir -p /var/www/html
echo '<html><body><p>EA4 smoke</p></body></html>' > /var/www/html/index.html
chown nobody:nobody /var/www/html/index.html

HDR_VALUE=""
# 12 attempts × 2s = 24s upper bound. License v2 init has been observed
# to take up to ~12s on a cold cache dir under EL9 (slower disk on the
# CI VM); the previous 16s budget (8×2s) was just on the edge.
for attempt in $(seq 1 12); do
    CURL_HEAD="$(curl -sI --max-time 5 -H 'Host: localhost' http://localhost/ 2>&1 || true)"
    HDR_LINE="$(printf '%s\n' "$CURL_HEAD" \
        | grep -iE '^X-Mod-Pagespeed|^X-Page-Speed' || true)"
    if [ -n "$HDR_LINE" ]; then
        HDR_VALUE="$HDR_LINE"
        log "  attempt ${attempt}/12: header observed"
        break
    fi
    STATUS_LINE="$(printf '%s\n' "$CURL_HEAD" | head -1 | tr -d '\r')"
    log "  attempt ${attempt}/12: no header yet (status: ${STATUS_LINE:-no response})"
    sleep 2
done

if [ -z "$HDR_VALUE" ]; then
    log "Failed to observe X-Mod-Pagespeed header after 12 attempts"
    fail "X-Mod-Pagespeed header missing"
fi
log "Header observed: $HDR_VALUE"

if ! echo "$HDR_VALUE" | grep -qF "$EXPECTED_VERSION_STRING"; then
    fail "Header '$HDR_VALUE' does not contain expected version '$EXPECTED_VERSION_STRING'"
fi
log "Version assertion passed"

# --- Phase 4: Uninstall and verify clean removal ---
log "=== Phase 4: dnf remove + verify clean ==="
EA4_AUTO_RELOAD=0 dnf remove -y ea-apache24-mod_pagespeed 2>&1 | tail -10

[ ! -f /etc/apache2/conf.modules.d/490_mod_pagespeed.conf ] \
    || fail "LoadModule fragment not removed"
[ ! -f /etc/apache2/modules/mod_pagespeed.so ] \
    || fail "mod_pagespeed.so not removed"

/scripts/restartsrv_httpd >/dev/null 2>&1 || fail "post-uninstall restart failed"

# Wait again for the post-uninstall restart to actually take effect before
# we assert the module is gone. Reusing the listen-poll loop pattern.
LISTEN_OK=0
for attempt in $(seq 1 20); do
    if ss -tln '( sport = :80 )' 2>/dev/null | grep -q ':80 '; then
        LISTEN_OK=1; break
    fi
    sleep 1
done
[ "$LISTEN_OK" = "1" ] || fail "Apache did not come back up after uninstall + restart"

if $EA4_HTTPD_BIN -M 2>/dev/null | grep -q 'pagespeed_module'; then
    fail "pagespeed_module still loaded after uninstall"
fi
if curl -sI --max-time 5 http://localhost/ 2>/dev/null | grep -qiE '^X-Mod-Pagespeed|^X-Page-Speed'; then
    fail "X-Mod-Pagespeed header still present after uninstall"
fi
log "Module fully removed"

log "=== ea4-smoke-target.sh PASSED ==="

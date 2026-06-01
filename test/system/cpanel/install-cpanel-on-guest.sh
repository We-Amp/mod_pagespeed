#!/usr/bin/env bash
# install-cpanel-on-guest.sh - install cPanel + WHM on a fresh AlmaLinux
# guest. Runs INSIDE the VM as root. Idempotent enough to re-run if the
# installer pauses for transient errors.
#
# The cPanel installer (the `sh latest` flow at
# https://docs.cpanel.net/installation-guide/installation-guide-linux/)
# requires:
#   - a fresh OS (no prior Apache/MySQL/Dovecot/Exim)
#   - a real-looking hostname (FQDN with at least three dots' worth of
#     structure)
#   - outbound HTTPS to securedownloads.cpanel.net + manage2.cpanel.net
#   - root login
#
# The cloud-init seed already set hostname=cp-el{8,9}.lab.local and added
# the loopback entry to /etc/hosts. The installer will refuse a hostname
# the OS resolves to 127.0.0.1 (must be 127.0.1.x or a real IP); we already
# placed 127.0.1.1 in /etc/hosts during cloud-init.
#
# This script INTENTIONALLY does not paste the trial license key anywhere.
# cPanel auto-activates a 15-day trial against the host's outbound IP the
# first time the installed cpanel-server reaches out to manage2.cpanel.net.
# That happens during the installer's "Final Steps" phase. The post-install
# verification step below confirms the license came back active.

set -euo pipefail

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
fail() { log "FAIL: $*"; exit 1; }

# --- Sanity ---
[ "$(id -u)" = "0" ] || fail "must run as root"
[ ! -d /etc/cpanel ] || fail "cPanel already installed at /etc/cpanel — refusing to re-run"

# --- Confirm hostname resolves ---
HN="$(hostname -f)"
log "hostname: $HN"
case "$HN" in
    *.*.*) ;;
    *) fail "hostname '$HN' is not a 3-part FQDN — cPanel will reject" ;;
esac
getent hosts "$HN" >/dev/null || fail "hostname '$HN' does not resolve via getent — fix /etc/hosts first"

# --- Confirm outbound HTTPS to cPanel infra ---
for h in securedownloads.cpanel.net manage2.cpanel.net httpupdate.cpanel.net; do
    if ! curl -sIL --max-time 10 "https://$h/" -o /dev/null; then
        fail "cannot reach https://$h — installer will fail"
    fi
done
log "outbound to cPanel hosts OK"

# --- Disk space ---
free_mb=$(df -m /home /usr /var 2>/dev/null | awk 'NR>1 { sum += $4 } END { print sum }')
log "free space across /home+/usr+/var: ${free_mb} MB"
[ "$free_mb" -gt 10000 ] || fail "need >10GB combined free; have ${free_mb}MB"

# --- Run cPanel installer ---
# This is the canonical `sh latest` invocation per the cPanel installation
# guide. It downloads + runs the bootstrapper which installs cpanel + WHM
# + all dependencies, configures services, and applies the trial license.
#
# Runtime: ~25-35 min on a fresh AlmaLinux 9 4vCPU/4GB. Lots of output;
# tee into a logfile we can inspect from the host if needed.
#
# We do NOT pass any license token — the installer auto-requests a trial
# from manage2.cpanel.net against this host's outbound IP.

cd /home
log "Downloading cPanel installer..."
curl -fsSL -o /home/cpanel-installer https://securedownloads.cpanel.net/latest

log "Starting cPanel install (long-running, may take 30+ min)..."
# `sh /home/cpanel-installer` is the recommended invocation. Pipe output
# to a logfile so we can inspect if it stalls.
sh /home/cpanel-installer 2>&1 | tee /var/log/cpanel-install.log

# --- Post-install verification ---
log "Verifying cPanel install..."
[ -d /usr/local/cpanel ] || fail "/usr/local/cpanel missing after install"
[ -x /usr/local/cpanel/cpanel ] || fail "cpanel binary missing"
[ -x /usr/local/cpanel/whostmgr/bin/whostmgr ] || fail "whostmgr binary missing"
[ -x /scripts/restartsrv_httpd ] || fail "/scripts/restartsrv_httpd missing"

# Install cpanel/elevate so the EL8 -> EL9 upgrade test (ea4-elevate-target.sh)
# can drive it. elevate-cpanel is NOT a yum/dnf package — it's a single
# perl script hosted on GitHub by the cpanel/elevate project. The
# canonical install path per https://cpanel.github.io/elevate/ is curl
# into /scripts/, which is also where /scripts/restartsrv_httpd lives so
# the elevate runbook can shell out by absolute path. Harmless on EL9
# (the elevate-cpanel binary refuses to elevate a host that's already on
# the target major), so we install on both VMs for snapshot symmetry.
log "Installing cpanel/elevate..."
if curl -sSLfo /scripts/elevate-cpanel \
       https://raw.githubusercontent.com/cpanel/elevate/release/elevate-cpanel; then
    chmod +x /scripts/elevate-cpanel
    log "elevate-cpanel installed: $(/scripts/elevate-cpanel --version 2>&1 | head -1)"
else
    log "::warning::elevate-cpanel download failed — elevate test will be skipped"
fi

# Confirm WHM is up.
systemctl is-active cpanel >/dev/null 2>&1 || fail "cpanel service not active"

# Confirm trial license came back active.
if [ -x /usr/local/cpanel/cpkeyclt ]; then
    /usr/local/cpanel/cpkeyclt 2>&1 | head -30 || true
fi
if [ -x /usr/local/cpanel/bin/whmapi1 ]; then
    /usr/local/cpanel/bin/whmapi1 get_license_info 2>&1 | head -20 || true
fi

# Confirm EA4 repo is configured (the installer pre-stages it).
if [ -f /etc/yum.repos.d/EA4.repo ]; then
    log "EA4 repo present: $(head -3 /etc/yum.repos.d/EA4.repo)"
else
    log "::warning::EA4 repo not at /etc/yum.repos.d/EA4.repo — check installer log"
fi

log "=== cPanel install complete ==="
log "Next: take snapshot '01-cpanel-clean-${HN%%.*}' from the host side."

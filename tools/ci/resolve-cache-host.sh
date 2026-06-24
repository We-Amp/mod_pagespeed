#!/usr/bin/env bash
#
# resolve-cache-host.sh -- print a *reachable* IP for cache-host.
#
# WHY THIS EXISTS
#   cache-host can carry more than one A-record on the LAN DNS: the live
#   address (currently 192.0.2.20) AND stale ones left behind by old
#   DHCP leases / extra interfaces (e.g. a dead 192.0.2.20). The old
#   idiom -- `getent hosts cache-host | awk '{print $1; exit}'` -- grabbed
#   the FIRST resolved address and assumed any resolved IP reaches the host.
#   That premise is false: when DNS lists a stale address first, every fetch
#   that pre-collapsed to that single IP died with "No route to host"
#   (a cross-runner artifact flake; the maintainer CI went red 2026-06-23).
#   (ssh/rsync given the *hostname* survive this because OpenSSH iterates all
#   resolved addresses -- it's only the pre-resolved-to-one-IP call sites and
#   `docker --add-host`, which needs a single IP, that break.)
#
# WHAT IT DOES
#   Probe TCP/22 on each resolved address in turn and print the first that
#   answers; fall back to the known-good default if none do. Always exits 0
#   and always prints something, so callers keep their own
#   `${VAR:-192.0.2.20}` safety net as a second line of defence.
#
# Override resolution entirely with CI_HUB_HOST=<ip-or-host>.
set -u

DEFAULT_IP="192.0.2.20"
PROBE_TIMEOUT="${CI_HUB_PROBE_TIMEOUT:-3}"

# Honour an explicit override (e.g. runner .env pinning) without probing.
if [ -n "${CI_HUB_HOST:-}" ]; then
  echo "${CI_HUB_HOST}"
  exit 0
fi

probe() {
  # Reachable if a TCP/22 connect completes within the timeout. /dev/tcp is a
  # bash builtin; if unavailable the connect "fails" and we move on -- safe.
  timeout "${PROBE_TIMEOUT}" bash -c "exec 3<>/dev/tcp/$1/22" 2>/dev/null
}

for ip in $(getent hosts cache-host 2>/dev/null | awk '$1 ~ /\./ {print $1}') "${DEFAULT_IP}"; do
  if probe "${ip}"; then
    echo "${ip}"
    exit 0
  fi
done

# Nothing answered -- emit the default so callers still get a usable value.
echo "${DEFAULT_IP}"

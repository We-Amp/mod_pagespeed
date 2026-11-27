#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# resolve-cache-host.sh -- print a *reachable* address for the CI cache/artifact
# hub.
#
# NOTHING environment-specific is stored in this file. The hub's name and its
# fallback address come from the environment; in GitHub Actions they are
# supplied by the repository variables CI_HUB_HOST / CI_HUB_IP (wired into each
# workflow's top-level `env:`). Keep in sync with the mod_pagespeed copy.
#
# WHY THIS EXISTS
#   The hub can carry more than one A-record on the LAN DNS: the live address
#   AND stale ones left behind by old DHCP leases / extra interfaces. The old
#   idiom -- `getent hosts <hub> | awk '{print $1; exit}'` -- grabbed the FIRST
#   resolved address and assumed any resolved IP reaches the host. That premise
#   is false: when DNS lists a dead record first, every fetch that pre-collapsed
#   to that single IP died with "No route to host" (CI #81 cross-runner artifact
#   flake; master ci.yml red 2026-06-23).
#   (ssh/rsync given the *hostname* survive this because OpenSSH iterates all
#   resolved addresses -- it's only the pre-resolved-to-one-IP call sites and
#   `docker --add-host`, which needs a single IP, that break.)
#
# WHAT IT DOES
#   Probe TCP/22 on each resolved address in turn and print the first that
#   answers; fall back to CI_HUB_IP if none do. Always exits 0 and always prints
#   something (possibly empty, if no fallback was supplied), so callers keep
#   their own `${VAR:-...}` safety net as a second line of defence.
#
# Environment
#   CI_HUB_ADDR       Skip resolution entirely and print this verbatim
#                     (use this to pin a runner via its .env).
#   CI_HUB_HOST       Hostname to resolve.
#   CI_HUB_IP         Fallback address.
#   CI_HUB_PROBE_TIMEOUT   TCP probe timeout in seconds (default 3).
set -u

HUB_NAME="${CI_HUB_HOST:-}"
FALLBACK_IP="${CI_HUB_IP:-}"
PROBE_TIMEOUT="${CI_HUB_PROBE_TIMEOUT:-3}"

# Honour an explicit override (e.g. runner .env pinning) without probing.
OVERRIDE="${CI_HUB_ADDR:-}"
if [ -n "${OVERRIDE}" ]; then
  echo "${OVERRIDE}"
  exit 0
fi

probe() {
  # Reachable if a TCP/22 connect completes within the timeout. /dev/tcp is a
  # bash builtin; if unavailable the connect "fails" and we move on -- safe.
  timeout "${PROBE_TIMEOUT}" bash -c "exec 3<>/dev/tcp/$1/22" 2>/dev/null
}

candidates=""
if [ -n "${HUB_NAME}" ]; then
  candidates="$(getent hosts "${HUB_NAME}" 2>/dev/null | awk '$1 ~ /\./ {print $1}')"
fi

for ip in ${candidates} ${FALLBACK_IP}; do
  if probe "${ip}"; then
    echo "${ip}"
    exit 0
  fi
done

# Nothing answered -- emit the fallback so callers still get a usable value.
echo "${FALLBACK_IP}"

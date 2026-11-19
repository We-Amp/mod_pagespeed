#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Make apt in CI survive a slow or intermittently failing Ubuntu archive.
#
# Usage: tools/ci/apt-robust.sh setup    write the retry/timeout config and,
#                                        when UBUNTU_MIRROR is set, point the
#                                        Ubuntu archive sources at it
#        tools/ci/apt-robust.sh update   setup, then apt-get update; fail when
#                                        any index failed to download, after
#                                        APT_RETRY_ATTEMPTS attempts
# Or source it and call apt_robust_setup / apt_update.
#
# Env:   UBUNTU_MIRROR   base URL of an Ubuntu archive mirror (for example
#                        http://mirror.example.org/ubuntu). Empty by default:
#                        the sources are left as they are. Applied only when
#                        /etc/os-release says ID=ubuntu, and only to
#                        archive.ubuntu.com and security.ubuntu.com entries in
#                        /etc/apt/sources.list and the deb822
#                        /etc/apt/sources.list.d/ubuntu.sources;
#                        ports.ubuntu.com (arm64) and third-party sources are
#                        never touched.
#        APT_RETRY_ATTEMPTS  attempts of apt-get update (default 2)
#        APT_RETRY_DELAY     seconds before each retry, as a space-separated
#                            list used in order; the last value repeats
#                            (default 20; for example "30 90 180")
#
# Runs as root or through sudo. Exit: 0 ok, 1 apt-get update failed on every
# attempt, 2 usage.

apt_robust_sudo() {
  if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

apt_robust_setup() {
  # Per-file retries cover a 503 or a reset on one index or package file; the
  # timeouts stop a stalled connection from hanging the job.
  printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' \
    | apt_robust_sudo tee /etc/apt/apt.conf.d/80-ci-retries >/dev/null || return 1

  local mirror="${UBUNTU_MIRROR:-}"
  [ -n "${mirror}" ] || return 0
  local id
  # shellcheck disable=SC1091
  id="$(. /etc/os-release 2>/dev/null && printf '%s' "${ID:-}")"
  [ "${id}" = ubuntu ] || return 0
  local f
  for f in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do
    [ -f "${f}" ] || continue
    apt_robust_sudo sed -i -E "s#https?://(archive|security)\.ubuntu\.com/ubuntu/?([[:space:]]|\$)#${mirror%/}/\\2#g" "${f}" || return 1
  done
  echo "apt-robust: Ubuntu archive sources point at ${mirror%/}/"
}

# apt-get update exits 0 when an index fails to download and then the
# install fails later with a misleading "Unable to locate package". Treat a
# failed index as a failure here, retry once, and show what failed.
apt_update() {
  local attempts="${APT_RETRY_ATTEMPTS:-2}" attempt out rc delay
  local -a delays
  read -r -a delays <<<"${APT_RETRY_DELAY:-20}"
  [ "${#delays[@]}" -gt 0 ] || delays=(20)
  for ((attempt = 1; attempt <= attempts; attempt++)); do
    rc=0
    out="$(apt_robust_sudo apt-get -qq -o DPkg::Lock::Timeout=120 update 2>&1)" || rc=$?
    if [ "${rc}" -eq 0 ] && ! printf '%s\n' "${out}" | grep -qE '^(W|E): (Failed to fetch|Some index files failed)'; then
      [ -z "${out}" ] || printf '%s\n' "${out}"
      return 0
    fi
    printf '%s\n' "${out}" | tail -n 12 >&2
    if [ "${attempt}" -lt "${attempts}" ]; then
      delay="${delays[attempt - 1]:-${delays[${#delays[@]} - 1]}}"
      echo "apt-robust: apt-get update failed (exit ${rc} or a failed index), attempt ${attempt}/${attempts}; retrying in ${delay}s" >&2
      sleep "${delay}"
    fi
  done
  echo "::error::apt-get update failed after ${attempts} attempts: an index could not be downloaded (see above)" >&2
  return 1
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  case "${1:-}" in
    setup) apt_robust_setup ;;
    update) apt_robust_setup && apt_update ;;
    *) echo "usage: $0 setup|update" >&2; exit 2 ;;
  esac
fi

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Install pinned syft + grype for the dependency-scan jobs, resiliently.
#
# Usage:
#   tools/ci/ensure-syft-grype.sh
#
# The versions are read from the SYFT_PIN/GRYPE_PIN lines of the checked-out
# tools/sbom/dep-scan.sh (see below); the workflow env is not consulted.
#
# Why this exists
# ---------------
# The dependency-scan jobs used to inline `curl … install.sh | sh` for syft and
# grype. The installer curls github.com release assets at job time, which
# intermittently fails — one run hit `received HTTP status=000`
# (a transient network/rate-limit blip) and then `unable to find tag=''`,
# reddening the whole "Blocking Dep Scan (npm + cargo)" gate over a single
# flaky fetch.
#
# Two layers of resilience, smallest sound change:
#
#   1. Runner-local persistent cache. These are persistent CI runners,
#      so we cache the pinned binaries in a stable per-runner dir
#      (CI_BIN_CACHE, default ~/.weamp/ci-bin) keyed by tool+version. Once a
#      version is installed there, every later run links it into the job-local
#      bin without touching the network at all. github.com is hit ONCE per new
#      pin, not once per run.
#
#   2. Retry plus checksum. When a cache miss does force a download,
#      tools/sbom/install-pinned-tool.sh fetches the release tarball with
#      retries and verifies its sha256 before extracting; a mismatch aborts.
#
# The job-local bin (RUNNER_TEMP/bin, or BIN_DIR override) is what ends up on
# PATH for the rest of the job; the cache is an install accelerator behind it.
set -euo pipefail

# The versions come from the checked-out tools/sbom/dep-scan.sh pins, which
# win over any SYFT_VERSION/GRYPE_VERSION in the environment: a pull request
# runs the workflow from the default branch but dep-scan.sh from its own
# checkout, so a static workflow env could never follow a pin bump.
DEP_SCAN="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tools/sbom/dep-scan.sh"
syft_pin="$(sed -n 's/^SYFT_PIN="\([^"]*\)".*/\1/p' "$DEP_SCAN" | head -n1)"
grype_pin="$(sed -n 's/^GRYPE_PIN="\([^"]*\)".*/\1/p' "$DEP_SCAN" | head -n1)"
if [ -z "$syft_pin" ] || [ -z "$grype_pin" ]; then
  echo "::error::could not parse SYFT_PIN/GRYPE_PIN from $DEP_SCAN"
  exit 1
fi
SYFT_VERSION="v${syft_pin}"
GRYPE_VERSION="v${grype_pin}"

# Job-local bin that goes on PATH for the rest of the job. Defaults to the
# GitHub-provided per-job RUNNER_TEMP; overridable for local testing.
BIN_DIR="${BIN_DIR:-${RUNNER_TEMP:?RUNNER_TEMP is required when BIN_DIR is unset}/bin}"

# Persistent per-runner cache. Survives across jobs/runs on persistent CI
# runners; the ~/.weamp dir is the established machine-local state location.
CI_BIN_CACHE="${CI_BIN_CACHE:-${HOME}/.weamp/ci-bin}"

# Retry knobs (overridable for tests / tuning), matching the house idiom.
DL_RETRIES="${DL_RETRIES:-1}"
DL_BACKOFF="${DL_BACKOFF:-5}"   # seconds between attempts

mkdir -p "$BIN_DIR" "$CI_BIN_CACHE"

# Put the job-local bin on PATH for subsequent steps (no-op outside GHA).
if [ -n "${GITHUB_PATH:-}" ]; then
  echo "$BIN_DIR" >> "$GITHUB_PATH"
fi
export PATH="$BIN_DIR:$PATH"

# Report the installed version of a tool on PATH as `vX.Y.Z`, or empty if absent.
have() {
  command -v "$1" >/dev/null 2>&1 && "$1" version 2>/dev/null | awk '/^Version:/{print "v"$2}'
}

# Download + install one tool at a pinned version into BIN_DIR through
# tools/sbom/install-pinned-tool.sh: the release tarball is fetched with
# retries and verified against the release's published sha256 file BEFORE it
# is extracted, and a mismatch aborts (no `install.sh | sh`, no ignored
# checksum failure).
install_tool() {
  local tool="$1" version="$2"
  INSTALL_RETRIES="$DL_RETRIES" INSTALL_RETRY_SLEEP="$DL_BACKOFF" \
    bash "$(dirname "$0")/../sbom/install-pinned-tool.sh" "$tool" "$version" "$BIN_DIR" \
    && [ "$(have "$tool")" = "$version" ] && return 0
  echo "::error::Failed to install ${tool} ${version}. The github.com release fetch may be down or rate-limited, or the checksum did not verify; re-run the job." >&2
  return 1
}

# Ensure one pinned tool is present in BIN_DIR, preferring the runner-local
# cache and only hitting the network on a cache miss.
ensure_tool() {
  local tool="$1" version="$2"
  local cached="${CI_BIN_CACHE}/${tool}-${version}"

  # Already the right version on PATH (e.g. pre-installed on the runner)? Done.
  if [ "$(have "$tool")" = "$version" ]; then
    echo "${tool} ${version} already on PATH — no install needed."
    return 0
  fi

  # Cache hit: link the pinned binary into the job-local bin. No network.
  if [ -x "$cached" ]; then
    ln -sf "$cached" "${BIN_DIR}/${tool}"
    if [ "$(have "$tool")" = "$version" ]; then
      echo "${tool} ${version} restored from runner-local cache (${cached}) — no network."
      return 0
    fi
    echo "::warning::cached ${tool} at ${cached} did not report ${version}; re-downloading." >&2
    rm -f "$cached" "${BIN_DIR}/${tool}"
  fi

  # Cache miss: download (with retry) into BIN_DIR, then populate the cache.
  install_tool "$tool" "$version"
  cp -f "${BIN_DIR}/${tool}" "$cached"
  echo "${tool} ${version} installed and cached at ${cached}."
}

ensure_tool syft  "$SYFT_VERSION"
ensure_tool grype "$GRYPE_VERSION"

syft version  | awk '/^Version:/{print "syft "$2}'
grype version | awk '/^Version:/{print "grype "$2}'

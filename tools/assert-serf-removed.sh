#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# assert-serf-removed.sh — guard that the Serf HTTP client stays removed.
#
# Serf (the APR-based async HTTP fetcher) was replaced across all ports by the
# libcurl-based CurlUrlAsyncFetcher. This guard fails (exit non-zero) if Serf
# creeps back in as a *built or linked artifact*. It deliberately checks only
# the load-bearing surfaces, not free-text comments — historical mentions like
# "we used to use Serf" or test-parity notes ("matching Serf") are fine and
# must not trip the gate.
#
# Invariants asserted:
#   1. No `serf` token in any linker version script (*.lds). A serf_* export
#      there resolves to nothing (dead) or, worse, would re-expose serf symbols.
#   2. No third_party/serf/ vendored tree.
#   3. No Serf repository_rule / http_archive in WORKSPACE or MODULE.bazel.
#
# Run from the repo root. Used by both developers and the serf-removal-guard CI
# job. Needs only grep + a POSIX shell.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
RC=0

# --- 1. version scripts must be serf-free -----------------------------------
LDS_HITS=$(grep -rniE 'serf' --include='*.lds' . 2>/dev/null || true)
if [ -n "${LDS_HITS}" ]; then
  echo "FAIL: 'serf' appears in a linker version script (*.lds) — serf is removed," >&2
  echo "      so the export is dead. Drop the line:" >&2
  echo "${LDS_HITS}" | sed 's/^/   /' >&2
  RC=1
else
  echo "OK: no serf in version scripts (*.lds)."
fi

# --- 2. no vendored serf tree -----------------------------------------------
if [ -d third_party/serf ]; then
  echo "FAIL: third_party/serf/ exists — serf was removed in favour of libcurl." >&2
  RC=1
else
  echo "OK: no third_party/serf/ vendored tree."
fi

# --- 3. no serf bazel repository --------------------------------------------
# Match a repo-rule declaration whose name is serf, e.g.
#   http_archive(name = "serf", ...)  /  native.new_local_repository(name="serf")
REPO_HITS=$(grep -rniE 'name[[:space:]]*=[[:space:]]*"serf"' \
  WORKSPACE MODULE.bazel 2>/dev/null || true)
if [ -n "${REPO_HITS}" ]; then
  echo "FAIL: a Bazel repository named 'serf' is declared — serf is removed:" >&2
  echo "${REPO_HITS}" | sed 's/^/   /' >&2
  RC=1
else
  echo "OK: no serf Bazel repository declared."
fi

if [ "${RC}" -ne 0 ]; then
  echo "=== serf-removal assertion FAILED ===" >&2
  exit 1
fi
echo "=== serf-removal assertion PASSED ==="

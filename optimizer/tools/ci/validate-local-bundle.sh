#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Decide whether a runner-local CI bundle (<dir>/.ci-artifacts) is complete and
# from the expected producer, instead of trusting a single file (#1825).
#
# Usage: validate-local-bundle.sh <bundle-dir> <expected-platform>
#
# Exit 0: every file stage-artifacts.sh installs is present and non-empty,
#         checksums.sha256 (when present) verifies, and metadata.json (when
#         present) names <expected-platform>.
# Exit 1: the bundle is partial or from another producer (reason on stderr).
#         The caller wipes it and re-fetches; this script never deletes.
#
# Why: a same-SHA bundle from another producer (the Release set has no
# ngx_pagespeed_module.so) or a half-copied bundle used to pass a bare
# `factory_worker exists` check and fail later at `install`.
#
# checksums.sha256 (every hub bundle ships one) catches a truncated-but-non-empty
# file, which an interrupted `rsync --inplace` upload leaves behind.
#
# metadata.json is optional on purpose: locally built bundles (arm64 periodic
# builds in the runner workspace) carry none. When it IS present its "platform"
# must match, which separates linux-x64 from linux-x64-periodic bundles.
set -euo pipefail

DIR="${1:-}"
PLATFORM="${2:-}"
if [ -z "$DIR" ] || [ -z "$PLATFORM" ]; then
  echo "usage: $0 <bundle-dir> <expected-platform>" >&2
  exit 2
fi

# Keep in sync with the unconditional `install` lines in stage-artifacts.sh.
REQUIRED_FILES=(factory_worker ngx_pagespeed_module.so)

for f in "${REQUIRED_FILES[@]}"; do
  if [ ! -s "$DIR/$f" ]; then
    echo "bundle incomplete: $DIR/$f missing or empty" >&2
    exit 1
  fi
done

if [ -f "$DIR/checksums.sha256" ]; then
  if command -v sha256sum >/dev/null 2>&1; then
    SUMCHK=(sha256sum -c --quiet)
  else
    SUMCHK=(shasum -a 256 -c --quiet)
  fi
  if ! (cd "$DIR" && "${SUMCHK[@]}" checksums.sha256 >/dev/null 2>&1); then
    echo "bundle checksum mismatch: $DIR/checksums.sha256 does not verify (truncated or partial copy)" >&2
    exit 1
  fi
fi

META="$DIR/metadata.json"
if [ -f "$META" ]; then
  LOCAL_PLATFORM=$({ grep -oE '"platform"[[:space:]]*:[[:space:]]*"[^"]*"' "$META" || true; } | head -1 | sed 's/.*"\([^"]*\)"$/\1/')
  if [ -n "$LOCAL_PLATFORM" ] && [ "$LOCAL_PLATFORM" != "$PLATFORM" ]; then
    echo "bundle platform mismatch: metadata says '${LOCAL_PLATFORM}', expected '${PLATFORM}'" >&2
    exit 1
  fi
fi
exit 0

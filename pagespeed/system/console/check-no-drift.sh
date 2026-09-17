#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Drift-guard: verify the checked-in admin_console.html is not stale relative to
# the console source.
#
# Why: the SPA bundle is checked in and embedded by Bazel (genrule -> data2c).
# Only the *release* vendor job rebuilds it; ordinary builds embed the committed
# artifact as-is. So editing the Svelte/TS source without re-running build.sh
# silently ships a stale bundle (this once shipped a "Free Trial" card after the
# source had dropped it).
#
# How: compare a SHA-256 over the build-determining source (console-srchash.sh)
# against the committed admin_console.html.srchash that build.sh writes. This is
# deterministic — unlike rebuilding and byte-comparing the minified bundle,
# which is NOT reproducible across environments (esbuild/toolchain) and flakes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SENTINEL="admin_console.html.srchash"

# The synced product-facts copy has its own drift guard against the canonical
# source; run it first so a stale facts copy is reported as such rather than
# surfacing later as an opaque srchash mismatch.
bash "$SCRIPT_DIR/check-product-facts.sh"

if [ ! -f "$SENTINEL" ]; then
  echo "::error::$SENTINEL is missing. Run 'pagespeed/system/console/build.sh' and commit it."
  exit 1
fi

expected="$(cat "$SENTINEL")"
actual="$(bash console-srchash.sh)"

if [ "$expected" != "$actual" ]; then
  echo "::error::admin_console.html is STALE relative to the console source."
  echo "The console source changed but the bundle was not rebuilt."
  echo "Run 'pagespeed/system/console/build.sh' and commit the rebuilt"
  echo "admin_console.html + admin_console.html.srchash."
  echo "  committed source hash: $expected"
  echo "  current  source hash:  $actual"
  exit 1
fi

echo "OK: admin_console.html source hash matches ($actual)."

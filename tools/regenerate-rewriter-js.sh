#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# regenerate-rewriter-js.sh — Rebuild pre-compiled JS from source.
#
# This script re-runs the JS bundler and data2c pipeline that was
# previously executed at build time via Bazel genrules.  The outputs are
# checked into net/instaweb/rewriter/generated/ so that normal builds do
# NOT require Node.js, npx or esbuild.
#
# Every source goes through the pinned esbuild in tools/js/ (config in
# tools/js/build-rewriter-js.mjs).
#
# Prerequisites:
#   - Node.js 22 / npm on PATH
#   - tools/js/node_modules (installed with `npm ci` on first use)
#   - A Bazel build of //net/instaweb/js:data2c (or pass DATA2C= to override)
#
# Usage:
#   tools/regenerate-rewriter-js.sh               # regenerate and show diff
#   tools/regenerate-rewriter-js.sh --check       # full regen + diff (needs node)
#   tools/regenerate-rewriter-js.sh --check-hash  # fast hash-only staleness check

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

REWRITER_DIR="$REPO_ROOT/net/instaweb/rewriter"
GEN_DIR="$REWRITER_DIR/generated"
JS_UTILS="$REPO_ROOT/net/instaweb/js/js_utils.js"
CLIENT_COMPAT="$REPO_ROOT/net/instaweb/js/client_compat.js"
JS_TOOLS_DIR="$REPO_ROOT/tools/js"

CHECK_ONLY=false
CHECK_HASH_ONLY=false
for arg in "$@"; do
    case "$arg" in
        --check)         CHECK_ONLY=true ;;
        --check-hash)    CHECK_HASH_ONLY=true ;;
        --esbuild-only)  ;;  # no-op, kept for older callers
        *) echo "ERROR: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# The rewriter sources, each built into a _dbg and an _opt asset (the same
# set compute_source_hash reads).
ESBUILD_FILES=(
    add_instrumentation.js
    client_domain_rewriter.js
    critical_css_beacon.js
    critical_css_loader.js
    critical_images_beacon.js
    dedup_inlined_images.js
    defer_iframe.js
    delay_images.js
    delay_images_inline.js
    deterministic.js
    extended_instrumentation.js
    js_defer.js
    lazyload_images.js
    local_storage_cache.js
    responsive_js.js
)

# Portable SHA-256: shasum on macOS, sha256sum on Linux
compute_source_hash() {
    cat \
        "$REWRITER_DIR/add_instrumentation.js" \
        "$REWRITER_DIR/client_domain_rewriter.js" \
        "$REWRITER_DIR/critical_css_beacon.js" \
        "$REWRITER_DIR/critical_css_loader.js" \
        "$REWRITER_DIR/critical_images_beacon.js" \
        "$REWRITER_DIR/dedup_inlined_images.js" \
        "$REWRITER_DIR/defer_iframe.js" \
        "$REWRITER_DIR/delay_images.js" \
        "$REWRITER_DIR/delay_images_inline.js" \
        "$REWRITER_DIR/deterministic.js" \
        "$REWRITER_DIR/extended_instrumentation.js" \
        "$REWRITER_DIR/js_defer.js" \
        "$REWRITER_DIR/lazyload_images.js" \
        "$REWRITER_DIR/local_storage_cache.js" \
        "$REWRITER_DIR/responsive_js.js" \
        "$JS_UTILS" \
        "$CLIENT_COMPAT" \
        "$JS_TOOLS_DIR/build-rewriter-js.mjs" \
        "$JS_TOOLS_DIR/package-lock.json" \
        "$SCRIPT_DIR/regenerate-rewriter-js.sh" \
    | if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -d' ' -f1
    else
        shasum -a 256 | cut -d' ' -f1
    fi
}

# Fast hash-only check: no Node.js needed
if [[ "$CHECK_HASH_ONLY" == true ]]; then
    ACTUAL=$(compute_source_hash)
    EXPECTED=$(tr -d '[:space:]' < "$GEN_DIR/.source-hash" 2>/dev/null || echo "MISSING")
    if [[ "$ACTUAL" == "$EXPECTED" ]]; then
        echo "Source hash matches — generated/ is up to date."
        exit 0
    else
        echo "ERROR: Source JS changed but generated/ is stale." >&2
        echo "  actual:   $ACTUAL" >&2
        echo "  expected: $EXPECTED" >&2
        echo "Run: tools/regenerate-rewriter-js.sh" >&2
        exit 1
    fi
fi

# Full regeneration requires node and npm (esbuild)
if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
    echo "ERROR: node/npm not found. Install Node.js to regenerate JS files." >&2
    exit 1
fi

# Build data2c if not provided
DATA2C="${DATA2C:-}"
if [[ -z "$DATA2C" ]]; then
    echo "==> Building data2c..."
    (cd "$REPO_ROOT" && bazel build //net/instaweb/js:data2c 2>&1 | tail -1)
    DATA2C="$REPO_ROOT/bazel-bin/net/instaweb/js/data2c"
fi

if [[ ! -x "$DATA2C" ]]; then
    echo "ERROR: data2c not found at $DATA2C" >&2
    exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# --------------------------------------------------------------------------
# Stage 1: esbuild, source JS to compiled JS
# --------------------------------------------------------------------------
ESBUILD_NAMES=()
echo "==> Compiling JS with esbuild..."
if [[ ! -d "$JS_TOOLS_DIR/node_modules/esbuild" ]]; then
    echo "==> Installing the pinned esbuild (npm ci in tools/js)..."
    npm ci --prefix "$JS_TOOLS_DIR" --no-fund --no-audit >/dev/null
fi
for src_file in "${ESBUILD_FILES[@]}"; do
    base="${src_file%.js}"
    echo "  $base (dbg + opt)"
    node "$JS_TOOLS_DIR/build-rewriter-js.mjs" \
        "$REWRITER_DIR/$src_file" "$TMPDIR" "$base"
    ESBUILD_NAMES+=("$base")
done

# --------------------------------------------------------------------------
# Stage 2: data2c — compiled JS → C++ string constants
# --------------------------------------------------------------------------
echo "==> Running data2c..."

# Run data2c from inside TMPDIR with relative paths: it embeds the input
# path in a header comment, and an absolute mktemp path there would make
# every .cc differ on every regen.
for name in "${ESBUILD_NAMES[@]}"; do
    (cd "$TMPDIR" && \
     "$DATA2C" --data_file="${name}_dbg.js" \
               --c_file="${name}.cc" \
               --varname="JS_${name}" && \
     "$DATA2C" --data_file="${name}_opt.js" \
               --c_file="${name}_opt.cc" \
               --varname="JS_${name}_opt")
done

# --------------------------------------------------------------------------
# Stage 3: Compare / install
# --------------------------------------------------------------------------
echo "==> Comparing with checked-in files..."
compute_source_hash > "$TMPDIR/.source-hash"
CHANGED=0
for f in "$TMPDIR"/*.cc "$TMPDIR"/*.js "$TMPDIR/.source-hash"; do
    fname="$(basename "$f")"
    target="$GEN_DIR/$fname"
    if [[ ! -f "$target" ]] || ! diff -q "$f" "$target" >/dev/null 2>&1; then
        echo "  CHANGED: $fname"
        CHANGED=1
    fi
done

if [[ "$CHANGED" -eq 0 ]]; then
    echo "==> All generated files are up to date."
    exit 0
fi

if [[ "$CHECK_ONLY" == true ]]; then
    echo "==> Generated files are out of date. Run without --check to update."
    exit 1
fi

echo "==> Installing updated files..."
mkdir -p "$GEN_DIR"
cp "$TMPDIR"/*.cc "$TMPDIR"/*.js "$TMPDIR/.source-hash" "$GEN_DIR/"

echo "==> Done. Review with: git diff net/instaweb/rewriter/generated/"

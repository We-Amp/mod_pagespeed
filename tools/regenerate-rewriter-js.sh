#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# regenerate-rewriter-js.sh — Rebuild pre-compiled JS from source.
#
# This script re-runs the JS compiler and data2c pipeline that was
# previously executed at build time via Bazel genrules.  The outputs are
# checked into net/instaweb/rewriter/generated/ so that normal builds do
# NOT require Node.js, npx, esbuild or the Google Closure Library.
#
# Assets are being moved from the Closure Compiler to esbuild one at a time:
# ESBUILD_FILES below go through the pinned esbuild in tools/js/ (config in
# tools/js/build-rewriter-js.mjs), every other file still goes through the
# Closure Compiler.
#
# Prerequisites:
#   - Node.js 22 / npm / npx on PATH
#   - npm package: google-closure-compiler (npx fetches the pinned version)
#   - tools/js/node_modules (installed with `npm ci` on first use)
#   - A Bazel build of //net/instaweb/js:data2c (or pass DATA2C= to override)
#
# Usage:
#   tools/regenerate-rewriter-js.sh               # regenerate and show diff
#   tools/regenerate-rewriter-js.sh --check       # full regen + diff (needs npx)
#   tools/regenerate-rewriter-js.sh --check-hash  # fast hash-only staleness check
#   tools/regenerate-rewriter-js.sh --esbuild-only [--check]
#                                  # only the esbuild assets (no Closure)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

REWRITER_DIR="$REPO_ROOT/net/instaweb/rewriter"
GEN_DIR="$REWRITER_DIR/generated"
JS_UTILS="$REPO_ROOT/net/instaweb/js/js_utils.js"
EXTERNS="$REPO_ROOT/net/instaweb/js/externs.js"
JS_TOOLS_DIR="$REPO_ROOT/tools/js"

CHECK_ONLY=false
CHECK_HASH_ONLY=false
ESBUILD_ONLY=false
for arg in "$@"; do
    case "$arg" in
        --check)         CHECK_ONLY=true ;;
        --check-hash)    CHECK_HASH_ONLY=true ;;
        --esbuild-only)  ESBUILD_ONLY=true ;;
        *) echo "ERROR: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# Every rewriter source, by base name (the same set compute_source_hash
# reads).
SOURCE_NAMES=(
    add_instrumentation client_domain_rewriter critical_css_beacon
    critical_css_loader critical_images_beacon dedup_inlined_images
    defer_iframe delay_images delay_images_inline deterministic
    extended_instrumentation js_defer lazyload_images local_storage_cache
    responsive_js
)

# Sources compiled with esbuild instead of the Closure Compiler.
ESBUILD_FILES=(
    add_instrumentation.js
    client_domain_rewriter.js
    critical_css_beacon.js
    dedup_inlined_images.js
    defer_iframe.js
    delay_images_inline.js
    deterministic.js
    extended_instrumentation.js
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
        "$EXTERNS" \
        "$JS_TOOLS_DIR/build-rewriter-js.mjs" \
        "$JS_TOOLS_DIR/package-lock.json" \
        "$SCRIPT_DIR/regenerate-rewriter-js.sh" \
    | if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -d' ' -f1
    else
        shasum -a 256 | cut -d' ' -f1
    fi
}

# Fast hash-only check: no Node.js/Closure Compiler needed
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

# Full regeneration requires node and npx (esbuild, Closure Compiler)
if ! command -v node >/dev/null 2>&1 || ! command -v npx >/dev/null 2>&1; then
    echo "ERROR: node/npx not found. Install Node.js to regenerate JS files." >&2
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
# Closure Compiler helpers
# --------------------------------------------------------------------------
# The compiler is pinned so that regeneration is deterministic: an unpinned
# npx resolves whatever is latest, which rewrites every generated asset on
# each regen. Bump deliberately, adopting the full diff.
CLOSURE_COMPILER_VERSION="20260311.0.0"

closure_compile() {
    local src="$1" output="$2" level="$3"
    shift 3
    local extra_flags=("$@")

    local format_flag=""
    if [[ "$level" == "SIMPLE" ]]; then
        format_flag="--formatting=PRETTY_PRINT"
    fi

    npx --yes "google-closure-compiler@$CLOSURE_COMPILER_VERSION" \
        --js "$src" \
        --js_output_file "$output" \
        "${extra_flags[@]}" \
        --compilation_level "$level" \
        --jscomp_off=checkVars \
        --generate_exports \
        "--output_wrapper=(function(){%output%})();" \
        $format_flag
    strip_closure_license "$output"
}

# js_utils.js is an ES module, imported by the esbuild assets. The files
# still compiled with the Closure Compiler goog.require('pagespeedutils'), so
# they get a derived copy instead: goog.provide('pagespeedutils') first, each
# `export const name = ` turned into `pagespeedutils.name = `, and each bare
# use of an exported name inside the file qualified as pagespeedutils.name.
# That is the form the file had before it became a module, so the Closure
# outputs stay the same. Fails if the module uses any other export form.
closure_js_utils() {
    local out="$1"
    if grep -n '^[[:space:]]*export' "$JS_UTILS" | \
       grep -v '^[0-9]*:export const [A-Za-z_][A-Za-z0-9_]* = ' >&2; then
        echo "ERROR: $JS_UTILS: use only 'export const name = ...;'" \
             "exports (see closure_js_utils)." >&2
        exit 1
    fi
    perl -0777 -ne '
        my @names = /^export const (\w+) = /mg;
        s/^export const (\w+) = /pagespeedutils.$1 = /mg;
        for my $n (@names) {
            s/(?<![\w.\$])\Q$n\E(?![\w\$])/pagespeedutils.$n/g;
        }
        print "goog.provide(\x27pagespeedutils\x27);\n\n", $_;
    ' "$JS_UTILS" > "$out"
}

# The pinned library's sources carry an @license header the compiler
# preserves into the output. Shipped assets must stay comment-free
# (StaticAssetManagerTest asserts this); attribution is retained in the
# pinned tools/closure/ checkout this script fetches.
strip_closure_license() {
    perl -0777 -pi -e \
        's{/\*\s*Copyright The Closure Library Authors\.\s*SPDX-License-Identifier: Apache-2\.0\s*\*/\n?}{}g' \
        "$1"
}

# --------------------------------------------------------------------------
# Stage 1a: esbuild, source JS to compiled JS
# --------------------------------------------------------------------------
# ${arr[@]+...} guard: macOS ships bash 3.2, where expanding an empty array
# under `set -u` is an unbound-variable error.
ESBUILD_NAMES=()
if [[ ${#ESBUILD_FILES[@]} -gt 0 ]]; then
    echo "==> Compiling JS with esbuild..."
    if [[ ! -d "$JS_TOOLS_DIR/node_modules/esbuild" ]]; then
        echo "==> Installing the pinned esbuild (npm ci in tools/js)..."
        npm ci --prefix "$JS_TOOLS_DIR" --no-fund --no-audit >/dev/null
    fi
    for src_file in "${ESBUILD_FILES[@]}"; do
        base="${src_file%.js}"
        echo "  $base (dbg + opt, esbuild)"
        node "$JS_TOOLS_DIR/build-rewriter-js.mjs" \
            "$REWRITER_DIR/$src_file" "$TMPDIR" "$base"
        ESBUILD_NAMES+=("$base")
    done
fi

# Tests whether $1 is one of ESBUILD_FILES.
is_esbuild_file() {
    local f
    for f in ${ESBUILD_FILES[@]+"${ESBUILD_FILES[@]}"}; do
        [[ "$f" == "$1" ]] && return 0
    done
    return 1
}

if [[ "$ESBUILD_ONLY" != true ]]; then
# --------------------------------------------------------------------------
# Stage 1b: Closure Compiler — source JS → compiled JS
# --------------------------------------------------------------------------
echo "==> Compiling JS with Closure Compiler..."

CLOSURE_JS_UTILS="$TMPDIR/closure/js_utils.js"
mkdir -p "$TMPDIR/closure"
closure_js_utils "$CLOSURE_JS_UTILS"

# Files that need dependency mode + the Closure Library.
#
# The library is pinned to the final upstream release (upstream is archived,
# so this never moves). Fetched on demand into tools/closure/ (gitignored);
# the commit hash is verified so a tampered or wrong checkout can't silently
# change the shipped JS.
CLOSURE_LIB_TAG="v20230802"
CLOSURE_LIB_COMMIT="ccf3f1dcd7258ff413406daa45bad79277d40afd"
CLOSURE_LIB_DIR="$REPO_ROOT/tools/closure/closure-library-$CLOSURE_LIB_TAG"
if [[ ! -d "$CLOSURE_LIB_DIR" ]]; then
    echo "==> Fetching Closure Library $CLOSURE_LIB_TAG..."
    mkdir -p "$REPO_ROOT/tools/closure"
    git clone --quiet --depth 1 --branch "$CLOSURE_LIB_TAG" \
        https://github.com/google/closure-library "$CLOSURE_LIB_DIR"
fi
ACTUAL_COMMIT="$(git -C "$CLOSURE_LIB_DIR" rev-parse HEAD)"
if [[ "$ACTUAL_COMMIT" != "$CLOSURE_LIB_COMMIT" ]]; then
    echo "ERROR: Closure Library at $CLOSURE_LIB_DIR is at commit" >&2
    echo "  $ACTUAL_COMMIT, expected $CLOSURE_LIB_COMMIT ($CLOSURE_LIB_TAG)." >&2
    echo "Remove the directory and re-run to fetch the pinned version." >&2
    exit 1
fi

# --- closure_compiler_gen files (with dependency mode, use closure library) ---
for entry in \
    "critical_css_loader.js:goog:pagespeed.CriticalCssLoader:$CLOSURE_JS_UTILS" \
    "critical_images_beacon.js:goog:pagespeed.CriticalImages:$CLOSURE_JS_UTILS" \
    "responsive_js.js:goog:pagespeed.Responsive:"; do

    IFS=':' read -r src_file entry_ns entry_point includes <<< "$entry"
    base="${src_file%.js}"
    src="$REWRITER_DIR/$src_file"

    include_flags=()
    if [[ -n "$includes" ]]; then
        include_flags+=(--js "$includes")
    fi

    # Collect closure library JS files
    cl_flags=()
    if [[ ! -d "$CLOSURE_LIB_DIR" ]]; then
        echo "ERROR: Closure Library missing at $CLOSURE_LIB_DIR" >&2
        exit 1
    fi
    while IFS= read -r f; do
        cl_flags+=(--js "$f")
    done < <(find "$CLOSURE_LIB_DIR" -name '*.js' ! -name '*_test.js' ! -name '*_perf.js' | sort)

    echo "  $base (dbg + opt, dependency mode)"
    # ${arr[@]+...} guard: macOS ships bash 3.2, where expanding an empty
    # array under `set -u` is an unbound-variable error.
    closure_compile "$src" "$TMPDIR/${base}_dbg.js" SIMPLE \
        ${include_flags[@]+"${include_flags[@]}"} \
        --entry_point "$entry_ns:$entry_point" \
        --dependency_mode PRUNE "${cl_flags[@]}"
    closure_compile "$src" "$TMPDIR/${base}_opt.js" ADVANCED \
        ${include_flags[@]+"${include_flags[@]}"} \
        --entry_point "$entry_ns:$entry_point" \
        --dependency_mode PRUNE "${cl_flags[@]}"
done

# --- closure_compiler_without_dependency_mode files ---
WITHOUT_DEP_FILES=(
)
WITH_DEP_FILES=(
    lazyload_images.js
    delay_images.js
    js_defer.js
    local_storage_cache.js
)

for src_file in ${WITHOUT_DEP_FILES[@]+"${WITHOUT_DEP_FILES[@]}"}; do
    if is_esbuild_file "$src_file"; then
        echo "ERROR: $src_file is listed for both esbuild and Closure" >&2
        exit 1
    fi
    base="${src_file%.js}"
    src="$REWRITER_DIR/$src_file"
    echo "  $base (dbg + opt)"
    closure_compile "$src" "$TMPDIR/${base}_dbg.js" SIMPLE \
        --externs "$EXTERNS"
    closure_compile "$src" "$TMPDIR/${base}_opt.js" ADVANCED \
        --externs "$EXTERNS"
done

for src_file in "${WITH_DEP_FILES[@]}"; do
    if is_esbuild_file "$src_file"; then
        echo "ERROR: $src_file is listed for both esbuild and Closure" >&2
        exit 1
    fi
    base="${src_file%.js}"
    src="$REWRITER_DIR/$src_file"
    echo "  $base (dbg + opt, with includes)"
    closure_compile "$src" "$TMPDIR/${base}_dbg.js" SIMPLE \
        --js "$CLOSURE_JS_UTILS" --externs "$EXTERNS"
    closure_compile "$src" "$TMPDIR/${base}_opt.js" ADVANCED \
        --js "$CLOSURE_JS_UTILS" --externs "$EXTERNS"
done
fi  # ESBUILD_ONLY

# --------------------------------------------------------------------------
# Stage 2: data2c — compiled JS → C++ string constants
# --------------------------------------------------------------------------
ALL_NAMES=("${SOURCE_NAMES[@]}")
if [[ "$ESBUILD_ONLY" == true ]]; then
    if [[ ${#ESBUILD_NAMES[@]} -eq 0 ]]; then
        echo "==> No esbuild assets configured; nothing to do."
        exit 0
    fi
    ALL_NAMES=("${ESBUILD_NAMES[@]}")
fi
echo "==> Running data2c..."

# Run data2c from inside TMPDIR with relative paths: it embeds the input
# path in a header comment, and an absolute mktemp path there would make
# every .cc differ on every regen.
for name in "${ALL_NAMES[@]}"; do
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

# Guard for --esbuild-only: .source-hash covers every input, but this mode
# rebuilds only the esbuild assets. Installing a new hash after a
# Closure-side input changed (a Closure-compiled source, js_utils.js or
# externs.js) would mark the stale Closure outputs as fresh and defeat the
# verify_generated_js genrule and --check-hash. So the new hash is only
# installed when those inputs are unchanged since the commit that last
# wrote .source-hash; otherwise the full regeneration is required.
if [[ "$ESBUILD_ONLY" == true ]] && \
   ! diff -q "$TMPDIR/.source-hash" "$GEN_DIR/.source-hash" >/dev/null 2>&1; then
    closure_inputs=("$JS_UTILS" "$EXTERNS")
    for name in "${SOURCE_NAMES[@]}"; do
        is_esbuild_file "$name.js" || closure_inputs+=("$REWRITER_DIR/$name.js")
    done
    hash_commit="$(git -C "$REPO_ROOT" log -1 --format=%H -- \
        "$GEN_DIR/.source-hash" 2>/dev/null || true)"
    if [[ -z "$hash_commit" ]] || \
       ! git -C "$REPO_ROOT" diff --quiet "$hash_commit" -- \
           "${closure_inputs[@]}"; then
        echo "ERROR: a Closure-compiled input changed since .source-hash was" >&2
        echo "  last written (or that could not be checked with git)." >&2
        echo "  --esbuild-only cannot refresh those outputs; run the full" >&2
        echo "  regeneration: tools/regenerate-rewriter-js.sh" >&2
        exit 1
    fi
fi

echo "==> Installing updated files..."
mkdir -p "$GEN_DIR"
cp "$TMPDIR"/*.cc "$TMPDIR"/*.js "$TMPDIR/.source-hash" "$GEN_DIR/"

echo "==> Done. Review with: git diff net/instaweb/rewriter/generated/"

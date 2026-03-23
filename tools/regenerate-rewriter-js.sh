#!/bin/bash
# regenerate-rewriter-js.sh — Rebuild pre-compiled JS from source.
#
# This script re-runs the Closure Compiler and data2c pipeline that was
# previously executed at build time via Bazel genrules.  The outputs are
# checked into net/instaweb/rewriter/generated/ so that normal builds do
# NOT require Node.js, npx, or the Google Closure Library.
#
# Prerequisites:
#   - Node.js / npx on PATH
#   - npm package: google-closure-compiler (npx will auto-install if needed)
#   - A Bazel build of //net/instaweb/js:data2c (or pass DATA2C= to override)
#
# Usage:
#   tools/regenerate-rewriter-js.sh               # regenerate and show diff
#   tools/regenerate-rewriter-js.sh --check       # full regen + diff (needs npx)
#   tools/regenerate-rewriter-js.sh --check-hash  # fast hash-only staleness check

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

REWRITER_DIR="$REPO_ROOT/net/instaweb/rewriter"
GEN_DIR="$REWRITER_DIR/generated"
JS_UTILS="$REPO_ROOT/net/instaweb/js/js_utils.js"
EXTERNS="$REPO_ROOT/net/instaweb/js/externs.js"

CHECK_ONLY=false
CHECK_HASH_ONLY=false
case "${1:-}" in
    --check)       CHECK_ONLY=true ;;
    --check-hash)  CHECK_HASH_ONLY=true ;;
esac

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

# Full regeneration requires npx (Closure Compiler)
if ! command -v npx >/dev/null 2>&1; then
    echo "ERROR: npx not found. Install Node.js to regenerate JS files." >&2
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
closure_compile() {
    local src="$1" output="$2" level="$3"
    shift 3
    local extra_flags=("$@")

    local format_flag=""
    if [[ "$level" == "SIMPLE" ]]; then
        format_flag="--formatting=PRETTY_PRINT"
    fi

    npx google-closure-compiler \
        --js "$src" \
        --js_output_file "$output" \
        "${extra_flags[@]}" \
        --compilation_level "$level" \
        --jscomp_off=checkVars \
        --generate_exports \
        "--output_wrapper=(function(){%output%})();" \
        $format_flag
}

# --------------------------------------------------------------------------
# Stage 1: Closure Compiler — source JS → compiled JS
# --------------------------------------------------------------------------
echo "==> Compiling JS with Closure Compiler..."

# Files that need dependency mode + closure library
CLOSURE_LIB_DIR="$REPO_ROOT/third_party/closure_library"
if [[ ! -d "$CLOSURE_LIB_DIR" ]]; then
    # Try bazel external
    CLOSURE_LIB_DIR="$(bazel info output_base 2>/dev/null)/external/closure_library" || true
fi

# --- closure_compiler_gen files (with dependency mode, use closure library) ---
for entry in \
    "critical_css_loader.js:goog:pagespeed.CriticalCssLoader:$JS_UTILS" \
    "critical_images_beacon.js:goog:pagespeed.CriticalImages:$JS_UTILS" \
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
    if [[ -d "$CLOSURE_LIB_DIR" ]]; then
        while IFS= read -r f; do
            cl_flags+=(--js "$f")
        done < <(find "$CLOSURE_LIB_DIR" -name '*.js' ! -name '*_test.js' ! -name '*_perf.js' | sort)
    fi

    echo "  $base (dbg + opt, dependency mode)"
    closure_compile "$src" "$TMPDIR/${base}_dbg.js" SIMPLE \
        "${include_flags[@]}" --entry_point "$entry_ns:$entry_point" \
        --dependency_mode PRUNE "${cl_flags[@]}"
    closure_compile "$src" "$TMPDIR/${base}_opt.js" ADVANCED \
        "${include_flags[@]}" --entry_point "$entry_ns:$entry_point" \
        --dependency_mode PRUNE "${cl_flags[@]}"
done

# --- closure_compiler_without_dependency_mode files ---
WITHOUT_DEP_FILES=(
    add_instrumentation.js
    client_domain_rewriter.js
    dedup_inlined_images.js
    defer_iframe.js
    delay_images_inline.js
    deterministic.js
    extended_instrumentation.js
)
WITH_DEP_FILES=(
    critical_css_beacon.js
    lazyload_images.js
    delay_images.js
    js_defer.js
    local_storage_cache.js
)

for src_file in "${WITHOUT_DEP_FILES[@]}"; do
    base="${src_file%.js}"
    src="$REWRITER_DIR/$src_file"
    echo "  $base (dbg + opt)"
    closure_compile "$src" "$TMPDIR/${base}_dbg.js" SIMPLE \
        --externs "$EXTERNS"
    closure_compile "$src" "$TMPDIR/${base}_opt.js" ADVANCED \
        --externs "$EXTERNS"
done

for src_file in "${WITH_DEP_FILES[@]}"; do
    base="${src_file%.js}"
    src="$REWRITER_DIR/$src_file"
    echo "  $base (dbg + opt, with includes)"
    closure_compile "$src" "$TMPDIR/${base}_dbg.js" SIMPLE \
        --js "$JS_UTILS" --externs "$EXTERNS"
    closure_compile "$src" "$TMPDIR/${base}_opt.js" ADVANCED \
        --js "$JS_UTILS" --externs "$EXTERNS"
done

# --------------------------------------------------------------------------
# Stage 2: data2c — compiled JS → C++ string constants
# --------------------------------------------------------------------------
echo "==> Running data2c..."

ALL_NAMES=(
    add_instrumentation client_domain_rewriter critical_css_beacon
    critical_css_loader critical_images_beacon dedup_inlined_images
    defer_iframe delay_images delay_images_inline deterministic
    extended_instrumentation js_defer lazyload_images local_storage_cache
    responsive_js
)

for name in "${ALL_NAMES[@]}"; do
    "$DATA2C" --data_file="$TMPDIR/${name}_dbg.js" \
              --c_file="$TMPDIR/${name}.cc" \
              --varname="JS_${name}"
    "$DATA2C" --data_file="$TMPDIR/${name}_opt.js" \
              --c_file="$TMPDIR/${name}_opt.cc" \
              --varname="JS_${name}_opt"
done

# --------------------------------------------------------------------------
# Stage 3: Compare / install
# --------------------------------------------------------------------------
echo "==> Comparing with checked-in files..."
CHANGED=0
for f in "$TMPDIR"/*.cc "$TMPDIR"/*.js; do
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
cp "$TMPDIR"/*.cc "$TMPDIR"/*.js "$GEN_DIR/"

echo "==> Updating .source-hash..."
compute_source_hash > "$GEN_DIR/.source-hash"

echo "==> Done. Review with: git diff net/instaweb/rewriter/generated/"

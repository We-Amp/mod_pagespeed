#!/bin/bash
# vendor-deps.sh - Populate a vendor directory with all dependencies for offline builds.
#
# Usage: tools/vendor-deps.sh [vendor-dir]
#
# This populates:
#   <vendor-dir>/repo-cache    - Bazel repository cache (all http_archive deps)
#   <vendor-dir>/cyclone       - Cyclone Cache source (git_repository)
#   <vendor-dir>/aom           - libaom source (git_repository; googlesource is
#                                git-only, so it cannot ride the repo-cache)
#
# After running this, builds can use --config=vendored for fully offline builds:
#   bazel build --config=vendored --config=clang-libstdcxx13 //...

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

VENDOR_DIR="${1:-vendor}"

# Resolve to absolute path
if [[ "$VENDOR_DIR" != /* ]]; then
    VENDOR_DIR="$REPO_ROOT/$VENDOR_DIR"
fi

REPO_CACHE="$VENDOR_DIR/repo-cache"
CYCLONE_DIR="$VENDOR_DIR/cyclone"
AOM_DIR="$VENDOR_DIR/aom"

mkdir -p "$REPO_CACHE"

cd "$REPO_ROOT"

# Use a temporary output base on the local (case-sensitive) filesystem.
# This avoids issues with case-insensitive filesystems (e.g., macOS-formatted SSDs)
# where external archives containing a 'build' dir collide with Bazel's 'BUILD' files.
VENDOR_OUTPUT_BASE="$(mktemp -d /tmp/bazel-vendor-XXXXXX)"
trap 'chmod -R u+w "$VENDOR_OUTPUT_BASE" 2>/dev/null; rm -rf "$VENDOR_OUTPUT_BASE"' EXIT

# -----------------------------------------------------------------------
# 1. Vendor git_repository deps FIRST (Cyclone)
#    These must exist before bazel fetch so we can use --override_repository
#    to bypass git clones and unblock http_archive dependency resolution.
# -----------------------------------------------------------------------

echo "==> Vendoring Cyclone..."
CYCLONE_COMMIT=$(grep 'CYCLONE_COMMIT' bazel/repositories.bzl | head -1 | sed 's/.*"\(.*\)".*/\1/' || true)
CYCLONE_BRANCH=$(grep -A2 'name = "cyclone"' bazel/repositories.bzl | grep 'branch' | sed 's/.*"\(.*\)".*/\1/' || true)

rm -rf "$CYCLONE_DIR"
if [ -n "$CYCLONE_COMMIT" ]; then
    echo "    Cyclone commit: $CYCLONE_COMMIT"
    git clone git@github.com:We-Amp/cyclone-cache.git "$CYCLONE_DIR"
    (cd "$CYCLONE_DIR" && git checkout "$CYCLONE_COMMIT")
elif [ -n "$CYCLONE_BRANCH" ]; then
    echo "    Cyclone branch: $CYCLONE_BRANCH"
    git clone --depth 1 --branch "$CYCLONE_BRANCH" git@github.com:We-Amp/cyclone-cache.git "$CYCLONE_DIR"
else
    echo "ERROR: Could not extract Cyclone commit or branch from bazel/repositories.bzl" >&2
    exit 1
fi

# Remove .git to save space and avoid nested repo issues
rm -rf "$CYCLONE_DIR/.git"

# Create WORKSPACE and BUILD files matching what git_repository would generate
# from the build_file_content in bazel/cyclone.bzl
touch "$CYCLONE_DIR/WORKSPACE"

# Extract the build_file_content from cyclone.bzl (between the triple-quote markers)
python3 -c "
import re
with open('bazel/cyclone.bzl') as f:
    content = f.read()
m = re.search(r'cyclone_build_rule\s*=\s*\"\"\"(.*?)\"\"\"', content, re.DOTALL)
if m:
    print(m.group(1))
else:
    raise SystemExit('ERROR: Could not extract cyclone_build_rule from bazel/cyclone.bzl')
" > "$CYCLONE_DIR/BUILD.bazel"

# libaom: git_repository on aomedia.googlesource.com (git-only; archive
# tarballs are not byte-stable, so it cannot be an http_archive and cannot
# ride the repo-cache). Vendored the same way as Cyclone so offline
# --config=vendored builds — notably the release nginx-distro containers,
# which ship without git — never need to clone it.
echo "==> Vendoring libaom..."
AOM_COMMIT=$(grep 'AOM_COMMIT' bazel/repositories.bzl | head -1 | sed 's/.*"\(.*\)".*/\1/' || true)
if [ -z "$AOM_COMMIT" ]; then
    echo "ERROR: Could not extract AOM_COMMIT from bazel/repositories.bzl" >&2
    exit 1
fi
echo "    aom commit: $AOM_COMMIT"
rm -rf "$AOM_DIR"
git clone https://aomedia.googlesource.com/aom "$AOM_DIR"
(cd "$AOM_DIR" && git checkout --detach "$AOM_COMMIT")
rm -rf "$AOM_DIR/.git"

touch "$AOM_DIR/WORKSPACE"

# Recreate the BUILD file git_repository would generate from build_file_content
# (_ALL_SRCS_BUILD_FILE in bazel/repositories.bzl)
python3 -c "
import re
with open('bazel/repositories.bzl') as f:
    content = f.read()
m = re.search(r'_ALL_SRCS_BUILD_FILE\s*=\s*\"\"\"(.*?)\"\"\"', content, re.DOTALL)
if m:
    print(m.group(1))
else:
    raise SystemExit('ERROR: Could not extract _ALL_SRCS_BUILD_FILE from bazel/repositories.bzl')
" > "$AOM_DIR/BUILD.bazel"

[ -f "$AOM_DIR/CMakeLists.txt" ] || {
    echo "ERROR: vendored aom looks wrong (no CMakeLists.txt at $AOM_DIR)" >&2
    exit 1
}

# -----------------------------------------------------------------------
# 2. Fetch all http_archive deps into the repository cache.
#    Use --override_repository for the git_repository deps we just vendored
#    so bazel doesn't try to git-clone them (which would fail without SSH).
#
#    We skip the envoy target because envoy's macros reference @rules_fuzzing
#    which isn't declared — this causes a loading error that aborts the fetch
#    before all deps are cached. The envoy http_archive itself is declared
#    in bazel/repositories.bzl and fetched below via 'bazel sync --only'.
# -----------------------------------------------------------------------

echo "==> Fetching dependencies into repository cache..."

OVERRIDE_FLAGS=(
    --override_repository=cyclone="$CYCLONE_DIR"
    --override_repository=aom_src="$AOM_DIR"
)

# Fetch deps for the Apache module — this pulls in the bulk of http_archive deps
# including boringssl, abseil, protobuf, and all transitive deps from
# protobuf_deps() (loaded unconditionally in WORKSPACE).
bazel --output_base="$VENDOR_OUTPUT_BASE" fetch \
    --repository_cache="$REPO_CACHE" \
    "${OVERRIDE_FLAGS[@]}" \
    //:libmod_pagespeed.so

# Fetch repos not reachable from the main target's build graph.
# The envoy http_archive is only needed by //pagespeed/envoy:envoy_pagespeed,
# but we can't fetch that target (envoy's macros reference @rules_fuzzing which
# isn't declared). Use 'bazel sync --only' to fetch just the repo rule.
bazel --output_base="$VENDOR_OUTPUT_BASE" sync \
    --repository_cache="$REPO_CACHE" \
    "${OVERRIDE_FLAGS[@]}" \
    --only=envoy

# Fetch IIS module deps (may fail analysis on non-Windows due to target_compatible_with,
# but all external repos are still cached during loading).
bazel --output_base="$VENDOR_OUTPUT_BASE" fetch \
    --repository_cache="$REPO_CACHE" \
    "${OVERRIDE_FLAGS[@]}" \
    //pagespeed/iis:pagespeed_iis.dll \
    || true

# Note: Some dependencies use repository_ctx.download() instead of http_archive
# (e.g., yq_windows_amd64). These bypass --repository_cache and are NOT vendored.
# Windows builds from the tarball still need network access for these small downloads.

# Verify the repo cache was populated and contains critical deps.
# Check a few key SHA256 hashes to catch incomplete caches early.
if [ ! -d "$REPO_CACHE/content_addressable" ]; then
    echo "ERROR: Repository cache was not populated (no content_addressable/ directory)" >&2
    echo "       Check bazel fetch output above for errors." >&2
    exit 1
fi

# Spot-check critical deps by SHA256 extracted from bazel/repositories.bzl.
# Dynamic extraction ensures these stay in sync when dep versions are bumped.
MISSING=0
check_dep() {
    local name="$1" sha="$2"
    if [ -z "$sha" ]; then
        echo "    WARNING: could not extract SHA for $name — skipping check" >&2
        return
    fi
    if [ ! -d "$REPO_CACHE/content_addressable/sha256/$sha" ]; then
        echo "    MISSING: $name ($sha)" >&2
        MISSING=$((MISSING + 1))
    fi
}
sha_of() { grep "$1" bazel/repositories.bzl | head -1 | sed 's/.*"\([0-9a-f]\{64\}\)".*/\1/'; }
check_dep "boringssl" "$(sha_of BORINGSSL_SHA)"
check_dep "abseil"    "$(sha_of _ABSEIL_SHA)"
check_dep "protobuf"  "$(sha_of PROTOBUF_SHA)"
check_dep "libpng"    "$(sha_of LIBPNG_SHA)"
check_dep "envoy"     "$(sha_of ENVOY_SHA)"

if [ "$MISSING" -gt 0 ]; then
    echo "ERROR: $MISSING critical deps missing from repository cache." >&2
    echo "       The bazel fetch may have failed silently. Check output above." >&2
    exit 1
fi

echo "    Repository cache: $(du -sh "$REPO_CACHE" | cut -f1)"

echo ""
echo "==> Vendor complete."
echo "    Vendor dir size: $(du -sh "$VENDOR_DIR" | cut -f1)"
echo "    Linux:   bazel build --config=vendored --config=clang-libstdcxx13 //..."
echo "    Windows: bazel build --config=vendored --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll"

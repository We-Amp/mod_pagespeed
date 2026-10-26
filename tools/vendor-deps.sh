#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

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
# 2. Fetch all external deps (BCR modules, their registry files and the
#    non_bcr_deps extension repos) into the repository cache.
#    Use --override_repository for the git_repository deps we just vendored
#    so bazel doesn't try to git-clone them (which would fail without SSH).
#    They are extension repos, so the override takes the canonical name.
# -----------------------------------------------------------------------

echo "==> Fetching dependencies into repository cache..."

OVERRIDE_FLAGS=(
    --override_repository=+non_bcr_deps+cyclone="$CYCLONE_DIR"
    --override_repository=+non_bcr_deps+aom_src="$AOM_DIR"
)

# Fetch deps for the Apache module: boringssl, abseil, protobuf and the bulk
# of the extension repos, plus the module resolution itself.
# --repo_contents_cache= : the repository cache is inside the source tree, and
# Bazel 9 would otherwise put its repo contents cache under it (it refuses a
# contents cache inside the main repo). A no-op on 8.8.1.
bazel --output_base="$VENDOR_OUTPUT_BASE" fetch \
    --repository_cache="$REPO_CACHE" \
    --repo_contents_cache= \
    "${OVERRIDE_FLAGS[@]}" \
    //:libmod_pagespeed.so

# Fetch the nginx module's deps. ed25519 (webbotauth) is used only by the nginx
# module, so the Apache fetch above never reaches it; without this fetch an
# offline nginx build fails on @ed25519. Not tolerated to fail: unlike the IIS
# target, the nginx module is a normal Linux target.
bazel --output_base="$VENDOR_OUTPUT_BASE" fetch \
    --repository_cache="$REPO_CACHE" \
    --repo_contents_cache= \
    "${OVERRIDE_FLAGS[@]}" \
    //pagespeed/nginx:ngx_pagespeed_module.so

# Fetch IIS module deps (may fail analysis on non-Windows due to target_compatible_with,
# but all external repos are still cached during loading).
bazel --output_base="$VENDOR_OUTPUT_BASE" fetch \
    --repository_cache="$REPO_CACHE" \
    --repo_contents_cache= \
    "${OVERRIDE_FLAGS[@]}" \
    //pagespeed/iis:pagespeed_iis.dll \
    || true

# The prebuilt protoc archives for every platform a release leg runs on. Proto
# toolchain resolution (--incompatible_enable_proto_toolchain_resolution in
# .bazelrc) fetches only the archive for the host that builds, so the Linux
# fetches above vendor just linux_x86_64. Fetch the other three explicitly, with
# the same sha256 pins protobuf carries, so a vendored build on Windows x64,
# Linux arm64 or macOS arm64 does not download protoc at build time.
PROTOC_REPOS=(
    prebuilt_protoc.linux_x86_64
    prebuilt_protoc.win64
    prebuilt_protoc.linux_aarch_64
    prebuilt_protoc.osx_aarch_64
)
PROTOC_FETCH=()
for r in "${PROTOC_REPOS[@]}"; do
    PROTOC_FETCH+=(--repo="@@protobuf++protoc+$r")
done
bazel --output_base="$VENDOR_OUTPUT_BASE" fetch \
    --repository_cache="$REPO_CACHE" \
    --repo_contents_cache= \
    "${OVERRIDE_FLAGS[@]}" \
    "${PROTOC_FETCH[@]}"

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
# ed25519 is pinned inline in its http_archive (nginx module only).
check_dep "ed25519"   "$(grep -A6 'name = "ed25519"' bazel/repositories.bzl | grep sha256 | head -1 | sed 's/.*"\([0-9a-f]\{64\}\)".*/\1/')"
# The four prebuilt protoc repos: Bazel checks each archive against the sha256
# in protobuf's integrity table when it fetches, so a repo that exists here was
# downloaded and verified (and its archive is in the repository cache).
for r in "${PROTOC_REPOS[@]}"; do
    f="$VENDOR_OUTPUT_BASE/external/protobuf++protoc+$r"
    [ -d "$f" ] || { echo "    MISSING: protoc repo $r" >&2; MISSING=$((MISSING + 1)); }
done

# Registry files: module resolution reads every MODULE.bazel, source.json and
# bazel_registry.json named in MODULE.bazel.lock through the repository cache,
# content-addressed by the sha256 the lock records. Assert that all of them are
# vendored so an offline resolution never needs the registry. "not found"
# entries are recorded negative lookups with no file behind them.
REG_REPORT=$(python3 -I - "$REPO_ROOT/MODULE.bazel.lock" "$REPO_CACHE" <<'PY'
import json, os, sys
lock, cache = sys.argv[1], sys.argv[2]
hashes = json.load(open(lock))["registryFileHashes"]
pinned = {u: s for u, s in hashes.items() if s != "not found"}
miss = [u for u, s in pinned.items()
        if not os.path.isfile(os.path.join(cache, "content_addressable", "sha256", s, "file"))]
for u in miss[:5]:
    print("    MISSING registry file: " + u, file=sys.stderr)
print(len(pinned), len(miss))
PY
)
read -r REG_PINNED REG_MISSING <<< "$REG_REPORT"
echo "    Registry files vendored: $((REG_PINNED - REG_MISSING)) of $REG_PINNED (missing: $REG_MISSING)"
MISSING=$((MISSING + REG_MISSING))

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

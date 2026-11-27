#!/usr/bin/env bash
# vendor-source.sh — Creates a vendored source tarball for offline builds.
# SPDX-License-Identifier: Apache-2.0
# Usage: ./vendor-source.sh [output-path]
# Default output: pagespeed-<VERSION>-vendor.tar.gz in the repo root.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERSION="${VERSION:-2.0.0-preview.1}"
OUTPUT="${1:-$REPO_ROOT/pagespeed-${VERSION}-vendor.tar.gz}"

cd "$REPO_ROOT"

echo "=== Vendoring dependencies ==="
# bazel vendor downloads all external dependencies for offline builds.
# --vendor_dir specifies where to store them within the repo.
# The targets below are the ones needed for NuGet native binaries:
#   libpagespeed.so  — shared library (C API for P/Invoke)
#   factory_worker   — background worker binary
bazel vendor \
  --vendor_dir=vendor \
  //lib/pagespeed:libpagespeed.so \
  //src/worker:factory_worker

echo "=== Creating source tarball ==="
# Create a tarball with source + vendor dir, excluding build artifacts
tar czf "$OUTPUT" \
  --exclude='.git' \
  --exclude='bazel-*' \
  --exclude='*.tar.gz' \
  --exclude='samples/aspnetcore/nupkg' \
  .

echo "=== Done ==="
ls -lh "$OUTPUT"
echo "Tarball: $OUTPUT"

#!/bin/bash
# Build the admin console SPA into a single index.html file.
# This script is run OUTSIDE of Bazel (before `bazel build`).
# The output dist/index.html is checked into the repo for hermeticity.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Ensure dependencies are installed
if [ ! -d "node_modules" ]; then
  echo "Installing dependencies..."
  pnpm install --frozen-lockfile 2>/dev/null || npm install
fi

# Stamp the version into the SPA. Release builds set VITE_APP_VERSION
# explicitly (to the release tag, e.g. v1.1.0-beta.6) so the stamp is exact
# instead of being derived from `git describe` (which can produce a stale or
# `-dirty` stamp when the SPA is built on a working tree that doesn't exactly
# match a tag). For developer workflows we fall back to `git describe`, then
# to "dev" if we're outside a git checkout. Picked up by Vite via
# import.meta.env.VITE_APP_VERSION.
VITE_APP_VERSION="${VITE_APP_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
export VITE_APP_VERSION
echo "Stamping version: ${VITE_APP_VERSION}"

echo "Building admin console SPA..."
npx vite build

# Verify output
if [ ! -f "dist/index.html" ]; then
  echo "ERROR: dist/index.html not produced" >&2
  exit 1
fi

SIZE=$(wc -c < dist/index.html)
echo "Built dist/index.html (${SIZE} bytes)"

# Copy to the checked-in location for Bazel
cp dist/index.html admin_console.html
echo "Copied to admin_console.html for Bazel embedding"

# Record a hash of the build-determining source so CI's drift-guard can detect
# a stale checked-in bundle without an (unreproducible) rebuild. The version
# stamp is intentionally excluded. See check-no-drift.sh.
bash "$SCRIPT_DIR/console-srchash.sh" > admin_console.html.srchash
echo "Wrote admin_console.html.srchash"

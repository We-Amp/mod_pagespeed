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

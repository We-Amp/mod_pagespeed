#!/bin/bash
# Print a deterministic SHA-256 over the console SPA's build-determining source
# inputs (the Svelte/TS source, entry HTML, vite/svelte/ts config, and the
# dependency manifests). Test-only files (e2e/, playwright.config.ts,
# vitest.config.ts) and the version stamp are intentionally excluded — they do
# not change the shipped bundle.
#
# Used by:
#   build.sh            — writes the result to admin_console.html.srchash
#   check-no-drift.sh   — recomputes and compares, to detect a checked-in
#                         bundle that is stale relative to its source.
#
# A source hash (not a rebuild-and-byte-compare) is used deliberately: the
# minified bundle is NOT reproducible from a clean install across environments
# (esbuild/toolchain differences), so comparing rebuilt bytes flakes in CI.
# Hashing the source is fully deterministic and catches the real foot-gun:
# editing the source without re-running build.sh.
set -euo pipefail

cd "$(cd "$(dirname "$0")" && pwd)"

paths=()
for p in src index.html vite.config.ts svelte.config.js tsconfig.json \
         tsconfig.node.json package.json pnpm-lock.yaml; do
  [ -e "$p" ] && paths+=("$p")
done

# Portable SHA-256: sha256sum on Linux (CI runners), shasum -a 256 on macOS
# (dev box). Both emit "<hash>  <path>", so the result is tool-independent.
if command -v sha256sum >/dev/null 2>&1; then
  HASH="sha256sum"
else
  HASH="shasum -a 256"
fi

find "${paths[@]}" -type f -print0 \
  | LC_ALL=C sort -z \
  | xargs -0 $HASH \
  | $HASH \
  | cut -d' ' -f1

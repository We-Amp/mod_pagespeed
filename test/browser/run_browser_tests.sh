#!/bin/bash
#
# Runs the browser-based client-JS integration tests in headless Chromium.
# Installs playwright + a chromium binary on first run (cached afterwards:
# node_modules/ here, browsers under ~/.cache/ms-playwright).
#
# Requirements: node >= 18 with npm/npx on PATH.

set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -d node_modules/playwright ]; then
  if [ -f package-lock.json ]; then
    npm ci --no-fund --no-audit
  else
    npm install --no-fund --no-audit
  fi
fi

# Fetch the chromium binary if this machine doesn't have it yet. Skip
# playwright's browser GC so this never deletes cached browsers that
# other projects on a shared machine still use.
PLAYWRIGHT_SKIP_BROWSER_GC=1 npx playwright install chromium

node critical_css_beacon_test.mjs

#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Runs the browser-based client-JS integration tests in headless Chromium.
# Installs playwright + a chromium binary on first run (cached afterwards:
# node_modules/ here, browsers under ~/.cache/ms-playwright).
#
# Requirements: node >= 18 with npm/npx on PATH.

set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -d node_modules/playwright ]; then
  npm ci --no-fund --no-audit
fi

# Fetch the chromium binary if this machine doesn't have it yet. Skip
# playwright's browser GC so this never deletes cached browsers that
# other projects on a shared machine still use. WebKit and Firefox are
# fetched only on request: the deferred-stylesheet loader test also runs in
# them then.
PLAYWRIGHT_SKIP_BROWSER_GC=1 npx playwright install chromium
if [ "${PAGESPEED_BROWSER_TESTS_WEBKIT:-0}" = "1" ]; then
  PLAYWRIGHT_SKIP_BROWSER_GC=1 npx playwright install webkit
fi
if [ "${PAGESPEED_BROWSER_TESTS_FIREFOX:-0}" = "1" ]; then
  PLAYWRIGHT_SKIP_BROWSER_GC=1 npx playwright install firefox
fi

node critical_css_beacon_test.mjs
node critical_css_loader_test.mjs
node add_instrumentation_test.mjs
node critical_images_beacon_test.mjs

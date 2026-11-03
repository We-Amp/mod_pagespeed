#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Builds the pinned framework fixtures into out/. --light skips the Next and
# Nuxt applications, which need several hundred MB of packages.
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -d node_modules/react ] || npm ci --no-fund --no-audit
rm -rf out
node build_fixtures.mjs
[ "${1:-}" = "--light" ] && exit 0
(cd next-app && { [ -d node_modules/next ] || npm ci --no-fund --no-audit; } && \
  NEXT_TELEMETRY_DISABLED=1 npx next build && cp -r out ../out/next)
(cd nuxt-app && { [ -d node_modules/nuxt ] || npm ci --no-fund --no-audit; } && \
  NUXT_TELEMETRY_DISABLED=1 npx nuxt generate && cp -rL .output/public ../out/nuxt)

#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# The admin console's tests as CI runs them: the unit tests, the type check,
# and the browser tests that load the committed bundle with mocked admin
# endpoints (e2e/*.mock.spec.ts -- no server, no build). Needs node with
# corepack (see .nvmrc). Run from anywhere.
set -euo pipefail
cd "$(cd "$(dirname "$0")" && pwd)"

export COREPACK_ENABLE_DOWNLOAD_PROMPT=0
corepack pnpm install --frozen-lockfile
corepack pnpm exec vitest run
corepack pnpm exec svelte-check --tsconfig ./tsconfig.json --fail-on-warnings
# Keep browsers that other projects on a shared machine still use.
PLAYWRIGHT_SKIP_BROWSER_GC=1 corepack pnpm exec playwright install chromium
corepack pnpm exec playwright test --project=mocked

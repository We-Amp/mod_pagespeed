// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node',
    // src/**/*.test.ts: future co-located unit tests.
    // tests/fixtures/**: checks on the scanner fixtures used by the visual suite.
    // test/**/*.test.ts: cross-file sync/contract checks (e.g. the
    //   ai-readability inline-gate drift check in test/sync/), kept out of
    //   src/ so Astro never tries to route/build them.
    include: ['src/**/*.test.ts', 'test/**/*.test.ts', 'tests/fixtures/**/*.test.ts'],
    // Without this, `vitest run` exits 1 on an empty test set and reds the CI
    // step. This does NOT mask failures: any matched test that fails still
    // exits non-zero. Astro pages are covered by Playwright e2e in tests/.
    passWithNoTests: true,
  },
});

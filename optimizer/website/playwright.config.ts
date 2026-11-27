// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig, devices } from '@playwright/test';
import { createHash } from 'node:crypto';

// The suite must test THIS checkout's code. A fixed port plus
// reuseExistingServer silently pointed every local run at whichever dev
// server already held :4321 -- usually another checkout or branch -- so
// results depended on what else was running (failures that vanished in
// isolation, a different test count between runs). Each checkout therefore
// gets its own port, derived from its path (override with PORT), and an
// already-running server is only reused on request (PW_REUSE_SERVER=1). If
// something foreign holds the port, Playwright now fails loudly instead of
// testing the wrong server. CI keeps 4321 (one checkout per runner).
// --ignore-lock keeps the server in the foreground: when astro detects an AI
// agent it otherwise daemonizes, Playwright cannot stop it, and the leaked
// server then collides with (or is silently reused by) the next run.
const PORT =
  process.env.PORT ??
  (process.env.CI
    ? '4321'
    : String(4500 + (createHash('sha1').update(process.cwd()).digest().readUInt16BE(0) % 500)));
// BASE_URL overrides the whole origin (then also set PORT for the dev server).
// Before any test, tests/global-setup.ts asserts that the server answering
// there serves THIS checkout (build-id tag, emitted by the dev server only; for
// a preview-server run see src/lib/build-id.mjs).
const BASE_URL = process.env.BASE_URL ?? `http://localhost:${PORT}`;

// Everything a run writes (failure screenshots, traces, the HTML report, the
// opt-in visual suite) goes under one ignored directory namespaced per run, so
// two runs from two checkouts, or two agents in one, never overwrite each
// other and a run never touches a committed file. PW_RUN_ID picks the
// namespace (default: the port); the visual spec reads PW_RUN_DIR. CI (one
// checkout per runner) keeps the default test-results/ and playwright-report/
// locations that the workflow uploads on failure.
const RUN_DIR = `test-output/${process.env.PW_RUN_ID ?? PORT}`;
process.env.PW_RUN_DIR = process.env.CI ? 'test-results' : RUN_DIR;

export default defineConfig({
  testDir: './tests',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  workers: process.env.CI ? 1 : undefined,
  outputDir: process.env.CI ? 'test-results' : `${RUN_DIR}/results`,
  reporter: [
    [
      'html',
      { outputFolder: process.env.CI ? 'playwright-report' : `${RUN_DIR}/report`, open: 'never' },
    ],
  ],
  globalSetup: './tests/global-setup.ts',
  use: {
    baseURL: BASE_URL,
    screenshot: 'only-on-failure',
  },
  // The fixtures test is a vitest file that lives next to the data it checks;
  // the visual suite is opt-in (VISUAL=1) and has its own project.
  testIgnore: ['**/fixtures/**'],
  projects: [
    {
      name: 'desktop',
      use: { ...devices['Desktop Chrome'] },
      testIgnore: ['**/fixtures/**', '**/visual/**'],
    },
    {
      name: 'mobile',
      use: { ...devices['Pixel 5'] },
      testMatch: /responsive\.spec\.ts/,
    },
    ...(process.env.VISUAL === '1'
      ? [
          {
            name: 'visual',
            testMatch: /visual\/.*\.visual\.spec\.ts/,
            fullyParallel: false,
            use: {
              ...devices['Desktop Chrome'],
              viewport: { width: 1280, height: 800 },
              colorScheme: 'dark' as const,
              contextOptions: { reducedMotion: 'reduce' as const },
              screenshot: 'off' as const,
            },
          },
        ]
      : []),
  ],
  webServer: {
    command: `npm run dev -- --port ${PORT} --ignore-lock`,
    url: BASE_URL,
    reuseExistingServer: !!process.env.PW_REUSE_SERVER,
    timeout: 30000,
  },
});

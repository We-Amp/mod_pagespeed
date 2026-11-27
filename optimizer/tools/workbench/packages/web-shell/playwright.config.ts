// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig } from '@playwright/test';

// In CI the Playwright container joins the Docker Compose network, so the
// console is served by nginx (http://nginx:8080) and no Vite dev server is
// needed.  Set PLAYWRIGHT_BASE_URL to override the default dev-server URL.
const baseURL = process.env.PLAYWRIGHT_BASE_URL || 'http://localhost:5173';
const useViteDevServer = !process.env.PLAYWRIGHT_BASE_URL;

export default defineConfig({
  globalSetup: './tests/e2e/global-setup.ts',
  testDir: './tests/e2e',
  // Default 30s for most tests; capture-heavy tests use test.slow() (3x).
  timeout: 30000,
  // Retry once for capture-heavy tests that may flake due to Chrome timing.
  retries: 1,
  // Run test files in parallel. Tests within a file stay sequential because
  // config/logs/capture tests mutate shared state (worker config, log buffer,
  // Chrome capture). Increase workers in CI if the machine has spare cores.
  fullyParallel: false,
  workers: process.env.CI ? 4 : undefined,
  use: {
    baseURL,
    screenshot: 'only-on-failure',
    trace: 'retain-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: { browserName: 'chromium' },
    },
  ],
  // Start the Vite dev server automatically if not already running.
  // Skipped in CI where nginx serves the built console directly.
  ...(useViteDevServer
    ? {
        webServer: {
          command: 'pnpm dev',
          url: 'http://localhost:5173',
          reuseExistingServer: true,
          timeout: 30000,
        },
      }
    : {}),
});

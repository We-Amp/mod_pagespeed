// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./e2e",
  timeout: 30000,
  retries: 0,
  use: {
    headless: true,
  },
  projects: [
    {
      // Self-contained: the committed bundle with mocked admin endpoints
      // (e2e/support/mock-admin.ts). No server is needed; CI runs these.
      name: "mocked",
      testMatch: /.*\.mock\.spec\.ts$/,
      use: { browserName: "chromium" },
    },
    {
      // Against a live console on localhost:8080 (vite dev or a real server).
      name: "live",
      testIgnore: /.*\.mock\.spec\.ts$/,
      use: { browserName: "chromium", baseURL: "http://localhost:8080" },
    },
  ],
});

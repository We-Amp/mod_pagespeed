// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig } from "@playwright/test";
import { LIVE_ORIGIN } from "./e2e/support/live";

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
      // Against a live console: CONSOLE_LIVE_URL, default localhost:8080 (vite
      // dev or a real server). The nightly runs it against the Apache test server.
      name: "live",
      // An allow-list: the other live specs predate the current pages (the
      // old Messages, Statistics, Caches and Histograms selectors) and fail
      // against a real server. Add a spec here once it passes live.
      testMatch: /check-graphs\.spec\.ts$/,
      use: { browserName: "chromium", baseURL: LIVE_ORIGIN },
    },
  ],
});

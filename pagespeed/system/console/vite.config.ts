// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig, type Plugin } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import { viteSingleFile } from "vite-plugin-singlefile";
import { CONSOLE_TITLE } from "./src/lib/data/product-facts-console";

// The document title derives from the product-facts single source (the synced
// copy in src/lib/data, re-exported by product-facts-console), so a product
// rename is a one-file edit upstream rather than a hunt through the SPA.
function consoleTitle(): Plugin {
  return {
    name: "console-title",
    transformIndexHtml(html: string) {
      return html.replace("%CONSOLE_TITLE%", CONSOLE_TITLE);
    },
  };
}

export default defineConfig({
  plugins: [svelte(), viteSingleFile(), consoleTitle()],
  resolve: {
    alias: {
      $lib: "/src/lib",
    },
  },
  server: {
    proxy: {
      "/stats_json": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/config": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/histograms": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/cache": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/console": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/message_history": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/graphs": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      // The daemon panels' read-only proxy endpoints; listed explicitly ahead
      // of the generic /v1/ rule so the console's daemon surface is visible
      // here at a glance. Same dev backend as everything else.
      "/v1/daemon/": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
      "/v1/": {
        target: "http://localhost:8080",
        rewrite: (path: string) => `/pagespeed_admin${path}`,
      },
    },
  },
  build: {
    target: "esnext",
    assetsInlineLimit: Infinity,
  },
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { sveltekit } from '@sveltejs/kit/vite';
import adapter from '@sveltejs/adapter-static';
import { defineConfig } from 'vitest/config';

// The workbench-demo worker requires a bearer token, and the
// browser has nowhere to hold one during local dev. The dev proxy plays the
// same role nginx plays in the demo stack — it holds the credential and adds
// it on the way through. Default kept in lockstep with
// tools/workbench-demo/entrypoint-worker-demo.sh.
const workerApiToken = process.env.PAGESPEED_API_TOKEN || 'workbench-demo-token';

export default defineConfig({
  plugins: [
    sveltekit({
      adapter: adapter({
        pages: 'dist',
        assets: 'dist',
        fallback: 'index.html',
      }),
      paths: {
        base: '/console',
      },
    }),
  ],
  test: {
    exclude: ['tests/e2e/**', 'node_modules/**'],
  },
  server: {
    port: 5173,
    proxy: {
      '/v1': {
        target: 'http://127.0.0.1:9880',
        changeOrigin: true,
        ws: true,
        headers: { Authorization: `Bearer ${workerApiToken}` },
      },
    },
  },
});

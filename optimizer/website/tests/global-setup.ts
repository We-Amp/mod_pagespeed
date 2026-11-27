// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { FullConfig } from '@playwright/test';
import { BUILD_ID } from '../src/lib/build-id.mjs';

// Runs after the webServer is up and before any test. The served home page must
// carry THIS checkout's build marker (<meta name="build-id">, see
// src/lib/build-id.mjs). Whatever else answers on the port (a sibling worktree's
// server, an older preview, an unrelated process) fails the whole run here,
// instead of producing passes or failures that belong to different code.
export default async function globalSetup(config: FullConfig) {
  const baseURL = config.projects[0].use.baseURL as string;
  let html = '';
  try {
    const res = await fetch(baseURL + '/');
    html = await res.text();
  } catch (err) {
    throw new Error(`fingerprint: cannot fetch ${baseURL}/ (${String(err)})`);
  }
  const m = /<meta\s+name="build-id"\s+content="([^"]*)"/.exec(html);
  if (!m || m[1] !== BUILD_ID) {
    throw new Error(
      `fingerprint mismatch at ${baseURL}: expected build-id ${BUILD_ID}, got ${m ? m[1] : 'none'}. ` +
        'The server on this port is not serving this checkout. Pick another PORT.',
    );
  }
}

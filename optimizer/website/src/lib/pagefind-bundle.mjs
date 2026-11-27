// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Where the Pagefind search bundle is served from. The bundle's script, wasm
// and loader files keep fixed names across Pagefind releases, and the server
// lets browsers cache script and style files for a long time, so a returning
// visitor could pair a cached old pagefind.js with a newly built index and get
// no search at all. Putting the installed Pagefind version in the directory
// gives every release its own URLs: pages (which are never cached) point at
// the new directory, and stale copies are simply never asked for again.
// scripts/build-search-index.mjs writes the bundle here, and astro.config.mjs
// bakes the same path into the search dialog.

import { readFileSync } from 'node:fs';

const pkg = JSON.parse(
  readFileSync(new URL('../../node_modules/pagefind/package.json', import.meta.url), 'utf8'),
);

if (!/^[0-9A-Za-z.+-]+$/.test(pkg.version ?? '')) {
  throw new Error(`pagefind-bundle: unexpected Pagefind version ${JSON.stringify(pkg.version)}`);
}

/** The Pagefind version the bundle is built with, e.g. "1.6.0". */
export const PAGEFIND_VERSION = pkg.version;

/** Site path of the bundle directory, with leading and trailing slash. */
export const PAGEFIND_BUNDLE_PATH = `/pagefind/v${PAGEFIND_VERSION}/`;

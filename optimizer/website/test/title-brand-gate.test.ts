// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// title-brand-gate.test.ts — a page title names the product once. The
// collection templates add a ` — mod_pagespeed` suffix (brandedTitle), which
// must not repeat a product name the title already carries. Runs after
// `npm run build` and skips itself otherwise (like anchor-gate.test.ts):
// no built page outside the /1.0/ archive has a <title> that contains the
// product name twice.

import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { PRODUCT_DISPLAY_NAME } from '../src/data/product-facts.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = path.join(WEBSITE_ROOT, 'dist/client');

// Hand-written titles on .astro pages that do not go through brandedTitle and
// are changed on their own.
const KNOWN: string[] = ['/alternatives/mod-pagespeed/'];

const BUILT = existsSync(DIST);
if (!BUILT) {
  console.warn(
    `[title-brand-gate] built output not found at ${DIST} — skipping. Run \`npm run build\` first.`,
  );
}

function pages(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const full = path.join(dir, name);
    if (statSync(full).isDirectory()) {
      if (path.relative(DIST, full) !== '1.0') out.push(...pages(full));
    } else if (name === 'index.html') out.push(full);
  }
  return out;
}

describe.skipIf(!BUILT)('built pages: the product name appears once in a title', () => {
  it('no title repeats the product name', () => {
    const repeated = pages(DIST)
      .map((file) => {
        const url =
          `/${path.relative(DIST, path.dirname(file)).split(path.sep).join('/')}/`.replace(
            /^\/\.?\/$/,
            '/',
          );
        const title = readFileSync(file, 'utf8').match(/<title>([^<]*)<\/title>/)?.[1] ?? '';
        return { url, title };
      })
      .filter(({ url }) => !KNOWN.includes(url))
      .filter(({ title }) => title.split(PRODUCT_DISPLAY_NAME).length > 2)
      .map(({ url, title }) => `${url}: ${title}`);
    expect(repeated).toEqual([]);
  });
});

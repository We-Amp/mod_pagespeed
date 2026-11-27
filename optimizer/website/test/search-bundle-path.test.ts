// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// search-bundle-path.test.ts — the Pagefind bundle is served from a directory
// that names the Pagefind version, so a browser holding a cached script from
// an older release is never handed it again. The first test needs no build;
// the others check the built pages and bundle when a build exists.

import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { PAGEFIND_BUNDLE_PATH, PAGEFIND_VERSION } from '../src/lib/pagefind-bundle.mjs';

const client = fileURLToPath(new URL('../dist/client', import.meta.url));
const built = existsSync(path.join(client, 'pagefind'));

function htmlFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      // The bundle itself and the frozen /1.0/ archive carry no search dialog.
      if (full === path.join(client, 'pagefind') || full === path.join(client, '1.0')) continue;
      out.push(...htmlFiles(full));
    } else if (entry.name.endsWith('.html')) out.push(full);
  }
  return out;
}

describe('Pagefind bundle path', () => {
  it('names the installed Pagefind version', () => {
    expect(PAGEFIND_BUNDLE_PATH).toBe(`/pagefind/v${PAGEFIND_VERSION}/`);
  });

  it.runIf(built)('built pages reference only the versioned bundle directory', () => {
    const pages = htmlFiles(client);
    expect(pages.length).toBeGreaterThan(0);
    let withDialog = 0;
    for (const file of pages) {
      const html = readFileSync(file, 'utf8');
      const rel = path.relative(client, file);
      for (const m of html.matchAll(/\/pagefind\/([^"'\s<>]*)/g)) {
        expect(
          `/pagefind/${m[1]}`.startsWith(PAGEFIND_BUNDLE_PATH),
          `${rel}: /pagefind/${m[1]}`,
        ).toBe(true);
      }
      const paths = [...html.matchAll(/data-bundle-path="([^"]*)"/g)].map((m) => m[1]);
      if (paths.length) withDialog++;
      for (const p of paths) expect(p, rel).toBe(PAGEFIND_BUNDLE_PATH);
    }
    expect(withDialog).toBeGreaterThan(0);
  });

  it.runIf(built)('the versioned directory holds the bundle and nothing sits beside it', () => {
    const dir = path.join(client, PAGEFIND_BUNDLE_PATH);
    for (const name of [
      'pagefind-ui.js',
      'pagefind-ui.css',
      'pagefind.js',
      'pagefind-entry.json',
    ]) {
      expect(statSync(path.join(dir, name)).size, name).toBeGreaterThan(0);
    }
    expect(readdirSync(dir).some((n) => /^wasm\..+\.pagefind$/.test(n))).toBe(true);
    expect(readdirSync(path.join(client, 'pagefind'))).toEqual([`v${PAGEFIND_VERSION}`]);
  });
});

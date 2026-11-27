// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// home-css-vars.test.ts — the home page's own stylesheet uses theme and
// Tailwind variables (--spacing, --text-3xl, --tw-border-style, ...) by hand.
// Tailwind emits a theme variable only while some utility uses it, so a
// variable can vanish from the shared stylesheet when its last other user
// goes, and the home rule that reads it then silently does nothing. This
// test (built output; runs after `npm run build`, skips itself otherwise)
// fails when the home page reads a variable that no stylesheet it loads
// defines. A var() with a fallback is not required to be defined; the
// variables inside its fallback are.

import { readFileSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));
const HOME = path.join(DIST, 'index.html');

/** Custom properties a stylesheet or markup defines. */
export function definedVars(text: string): Set<string> {
  const out = new Set<string>();
  for (const m of text.matchAll(/(?:^|[{;\s"'])(--[\w-]+)\s*:/g)) out.add(m[1]);
  for (const m of text.matchAll(/@property\s+(--[\w-]+)/g)) out.add(m[1]);
  return out;
}

/** Custom properties read with var() and no fallback. */
export function requiredVars(css: string): Set<string> {
  const out = new Set<string>();
  for (const m of css.matchAll(/var\(\s*(--[\w-]+)\s*\)/g)) out.add(m[1]);
  return out;
}

function homeSheets(html: string): { own: string; all: string } {
  const links = [...html.matchAll(/<link rel="stylesheet" href="\/(_astro\/[^"]+\.css)"/g)].map(
    (m) => m[1],
  );
  const sheet = (l: string) => readFileSync(path.join(DIST, l), 'utf8');
  const inline = [...html.matchAll(/<style[^>]*>([\s\S]*?)<\/style>/g)].map((m) => m[1]);
  // The page's own styles: every sheet but the shared layout one, linked or
  // inlined (Astro inlines a small sheet into the page).
  const own = [...links.filter((l) => !/\/BaseLayout\./.test(l)).map(sheet), ...inline].join('\n');
  return { own, all: [...links.map(sheet), html].join('\n') };
}

describe('variable check (probe)', () => {
  it('flags a variable that is read but not defined', () => {
    const css =
      ':root{--a:1px}.x{gap:var(--a);font-size:var(--missing);line-height:var(--opt,var(--b))}';
    const defined = definedVars(css);
    const missing = [...requiredVars(css)].filter((v) => !defined.has(v));
    expect(missing.sort()).toEqual(['--b', '--missing']);
  });
});

describe.skipIf(!existsSync(HOME))('home page stylesheet (after npm run build)', () => {
  it('every variable the home page styles read is defined in the CSS it loads', () => {
    const { own, all } = homeSheets(readFileSync(HOME, 'utf8'));
    expect(own.length, 'the home page loads a stylesheet of its own').toBeGreaterThan(0);
    const defined = definedVars(all);
    const missing = [...requiredVars(own)].filter((v) => !defined.has(v));
    expect(missing).toEqual([]);
  });
});

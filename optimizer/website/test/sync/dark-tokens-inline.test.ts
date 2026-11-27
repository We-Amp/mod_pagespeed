// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// dark-tokens-inline.test.ts — SYNC GUARD between the dark token set in
// src/styles/global.css (`.dark { … }`) and the inline critical-CSS flash
// guard in src/layouts/BaseLayout.astro (`html.dark { … }`). The inline block
// must carry the complete dark token list, value for value: the production
// critical-CSS extractor drops `.dark` rules, and this inline copy is what
// keeps token-based utilities from resolving to light values on first paint.
// Any name or value difference fails the build.

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';

const GLOBAL_CSS = resolve(__dirname, '../../src/styles/global.css');
const BASE_LAYOUT = resolve(__dirname, '../../src/layouts/BaseLayout.astro');

/** Extract the `--name: value` declarations of every block for `selector`. */
function tokenMap(source: string, selector: string): Map<string, string> {
  const map = new Map<string, string>();
  // Block bodies here contain no nested braces (custom properties only), so a
  // non-greedy brace match is exact for this file.
  const re = new RegExp(`(?:^|[}\\s])${selector.replace('.', '\\.')}\\s*\\{([^}]*)\\}`, 'gm');
  for (const block of source.matchAll(re)) {
    for (const decl of block[1].matchAll(/(--[a-z0-9-]+)\s*:\s*([^;]+);/gi)) {
      const name = decl[1].toLowerCase();
      const value = decl[2].trim();
      if (map.has(name) && map.get(name) !== value) {
        throw new Error(`conflicting values for ${name} in ${selector} blocks`);
      }
      map.set(name, value);
    }
  }
  return map;
}

describe('inline dark tokens (critical-CSS flash guard)', () => {
  const cssTokens = tokenMap(readFileSync(GLOBAL_CSS, 'utf8'), '.dark');
  const inlineTokens = tokenMap(readFileSync(BASE_LAYOUT, 'utf8'), 'html.dark');

  it('the .dark block in global.css declares a full token set', () => {
    expect(cssTokens.size).toBeGreaterThan(20);
  });

  it('BaseLayout.astro inlines exactly the same token names', () => {
    expect([...inlineTokens.keys()].sort()).toEqual([...cssTokens.keys()].sort());
  });

  it('every inline value equals the .dark value', () => {
    for (const [name, value] of cssTokens) {
      expect(inlineTokens.get(name), name).toBe(value);
    }
  });
});

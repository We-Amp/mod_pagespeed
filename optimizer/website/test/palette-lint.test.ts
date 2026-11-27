// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// palette-lint.test.ts — the palette ban, enforced. The site's palette
// moved to the instrument tokens (see src/styles/global.css): no blue-family
// Tailwind utilities and no blue rgba() literals anywhere in src/ or the
// top-level public/*.svg marks, and no shade of any other stock Tailwind
// palette either (stone-, emerald-, amber-, red-, green-, ...), whether as a
// utility, a --color-* variable or a comment that points at one. Colour comes
// from the tokens; a page that needs a colour the tokens do not carry keeps
// it in its own scoped style.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const websiteRoot = fileURLToPath(new URL('..', import.meta.url));

const SCAN_EXTENSIONS = new Set([
  '.astro',
  '.css',
  '.ts',
  '.tsx',
  '.js',
  '.mjs',
  '.cjs',
  '.svg',
  '.md',
  '.mdx',
]);

function scanFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) {
      if (entry === 'node_modules' || entry.startsWith('.')) continue;
      out.push(...scanFiles(full));
    } else if (SCAN_EXTENSIONS.has(entry.slice(entry.lastIndexOf('.')))) {
      out.push(full);
    }
  }
  return out;
}

const files = [
  ...scanFiles(join(websiteRoot, 'src')),
  // The top-level public marks only; public/1.0 is the frozen archive and
  // public/images carries third-party logos with their own brand colours.
  ...readdirSync(join(websiteRoot, 'public'))
    .filter((f) => f.endsWith('.svg'))
    .map((f) => join(websiteRoot, 'public', f)),
];

// A Tailwind utility from the banned cool families: blue-*, sky-*, indigo-*,
// purple-* (the full shade number, so the violation listing reads blue-500,
// not blue-5).
const BANNED_UTILITY = /\b(?:blue|sky|indigo|purple)-\d+/;

// A decimal-channel rgba() whose blue channel clearly dominates — e.g. the
// retired accent rgba(29, 78, 216, …). Near-neutral overlays like
// rgba(7, 8, 10, 0.72) stay legal: dominance has to exceed 24 per channel.
const RGBA = /\brgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,/g;

// Every other stock Tailwind palette, by shade (50, 100-900, 950), so ids
// such as arrow-stone-20 and names such as prose-stone stay legal.
const STOCK_PALETTE =
  /\b(?:slate|gray|zinc|neutral|stone|red|orange|amber|yellow|lime|green|emerald|teal|cyan|violet|fuchsia|pink|rose)-(?:50|[1-9]00|950)\b/g;

interface Violation {
  file: string;
  line: number;
  match: string;
}

const violations: Violation[] = [];
const stockViolations: Violation[] = [];

for (const file of files) {
  const rel = relative(websiteRoot, file);
  const text = readFileSync(file, 'utf8');
  const lines = text.split('\n');

  lines.forEach((line, i) => {
    const util = line.match(BANNED_UTILITY);
    if (util) {
      violations.push({ file: rel, line: i + 1, match: util[0] });
    }
    for (const m of line.matchAll(RGBA)) {
      const [r, g, b] = [Number(m[1]), Number(m[2]), Number(m[3])];
      if (b > r + 24 && b > g + 24) {
        violations.push({ file: rel, line: i + 1, match: m[0] });
      }
    }
    for (const m of line.matchAll(STOCK_PALETTE)) {
      stockViolations.push({ file: rel, line: i + 1, match: m[0] });
    }
  });
}

describe('palette lint', () => {
  it('no blue-family utilities or blue rgba() literals in src/ or public/*.svg', () => {
    const listing = violations.map((v) => `  ${v.file}:${v.line}  ${v.match}`).join('\n');
    expect(violations, `cool-palette violations:\n${listing}`).toEqual([]);
  });

  it('no stock Tailwind palette shades in src/ or public/*.svg', () => {
    const listing = stockViolations.map((v) => `  ${v.file}:${v.line}  ${v.match}`).join('\n');
    expect(stockViolations, `stock-palette uses (use a token):\n${listing}`).toEqual([]);
  });
});

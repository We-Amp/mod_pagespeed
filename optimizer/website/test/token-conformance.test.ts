// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// token-conformance.test.ts — the utilities that bypass the design tokens in
// src/styles/global.css stay out of the source:
//   - rounded-xl / rounded-2xl: surfaces use rounded-card (--radius-card);
//   - shadow, shadow-2xs ... shadow-2xl, shadow-inner, drop-shadow-*: depth comes from finish
//     (hairline frame, specular edge), never a drop shadow;
//   - text-[Npx] below 12 px: the smallest text role is .text-micro (12 px).
// Markdown content is not scanned: these are markup and style rules.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const websiteRoot = fileURLToPath(new URL('..', import.meta.url));

const SCAN_EXTENSIONS = new Set(['.astro', '.css', '.ts', '.tsx', '.js', '.mjs', '.cjs']);

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

// The scanner pages keep their layout, card radius included, until they are
// redesigned as a set; only the radius rule skips them.
const RADIUS_EXEMPT = [
  'src/pages/analyze.astro',
  'src/pages/ai-readability/',
  'src/pages/ai-crawler-cost',
  'src/components/scan/',
  'src/scripts/scan/',
];

interface Rule {
  name: string;
  pattern: RegExp;
  exempt?: string[];
}

const RULES: Rule[] = [
  {
    name: 'rounded-xl/2xl (use rounded-card)',
    pattern: /(?<![\w-])rounded-(?:[trblse]{1,2}-)?(?:xl|2xl|3xl)(?![\w-])/g,
    exempt: RADIUS_EXEMPT,
  },
  {
    name: 'drop-shadow utility',
    pattern: /(?<![\w-])(?:drop-)?shadow-(?:2xs|xs|sm|md|lg|xl|2xl|inner)(?![\w-])/g,
  },
  {
    // Bare `shadow` only where it can be a class: next to a quote, a variant
    // colon or another class on a line that sets classes, so the word in
    // comments and prose stays legal.
    name: 'drop-shadow utility',
    pattern: /(?<=class(?:Name|List)?=?[^\n]*?["'`\s:])shadow(?=["'`\s])(?![^\n]*?\*\/)/g,
  },
  {
    name: 'text below 12 px (use text-micro)',
    pattern: /(?<![\w-])text-\[(?:\d|1[01])(?:\.\d+)?px\]/g,
  },
];

interface Violation {
  rule: string;
  file: string;
  line: number;
  match: string;
}

const violations: Violation[] = [];

for (const file of scanFiles(join(websiteRoot, 'src'))) {
  const rel = relative(websiteRoot, file).split(sep).join('/');
  const lines = readFileSync(file, 'utf8').split('\n');
  lines.forEach((line, i) => {
    for (const rule of RULES) {
      if (rule.exempt?.some((prefix) => rel.startsWith(prefix))) continue;
      for (const m of line.matchAll(rule.pattern)) {
        violations.push({ rule: rule.name, file: rel, line: i + 1, match: m[0] });
      }
    }
  });
}

describe('token conformance', () => {
  it('no radius, shadow or sub-12px text utilities that bypass the tokens', () => {
    const listing = violations
      .map((v) => `  ${v.file}:${v.line}  ${v.match}  [${v.rule}]`)
      .join('\n');
    expect(violations, `token bypasses:\n${listing}`).toEqual([]);
  });

  it('the patterns still fire', () => {
    const probes = [
      'class="rounded-2xl border"',
      'class="rounded-xl"',
      'class="open:shadow-sm"',
      'class="card shadow-lg"',
      'class="card shadow"',
      'class="shadow-inner"',
      'class="hover:drop-shadow-md"',
      'class="text-[10px]"',
      'class="text-[11px]"',
    ];
    for (const probe of probes) {
      expect(
        RULES.some((r) => new RegExp(r.pattern.source).test(probe)),
        probe,
      ).toBe(true);
    }
    const legal = [
      'class="rounded-card rounded-chip rounded-lg"',
      'box-shadow: inset 0 0 0 1px var(--color-border);',
      'class="shadow-none"',
      'class="text-[12px] text-xs"',
      'shadow DOM',
      '// shadow that page during prerender',
      'class="shadow-[inset_0_1px_0_0_var(--color-border)]"',
    ];
    for (const ok of legal) {
      expect(
        RULES.some((r) => new RegExp(r.pattern.source).test(ok)),
        ok,
      ).toBe(false);
    }
  });
});

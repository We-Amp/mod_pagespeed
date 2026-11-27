// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// header-version-gate.test.ts — the modules send the generic value `on` in
// their PageSpeed response header by default and disclose the version only with
// the server-wide ExposeVersionInHeaders option. A docs example that shows a
// version (or a version placeholder) in X-Mod-Pagespeed / X-Page-Speed /
// X-PageSpeed therefore misleads, unless the opt-in is named on the same line or within
// NEAR lines of it. A mention elsewhere in the file does not exempt anything.
// This gate scans the authored docs, pages, components and src/lib.
//
// Not scanned: release notes, blog posts and the legacy 1.0 pages (historical
// facts about older releases), and src/content/releases-* manifests.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const SCAN_DIRS = [
  'src/content/docs',
  'src/pages',
  'src/components',
  'src/data/reference/notes',
  'src/lib',
];
const SKIP = [/release-notes/i, /\/blog\//, /\/1\.0\//, /^src\/content\/docs\/1\.0/];
const OPT_IN = 'ExposeVersionInHeaders';
const NEAR = 3;
// A legacy rollback value (the pre-2.x module) is a historical fact, not an example of
// current output.
const LEGACY_VALUE = /1\.13\.35\.2-0/;

// Header name, separator, then a version number, a `<version>` placeholder or a
// template interpolation of a version constant.
const VERSION_IN_HEADER =
  /\bx-(?:mod-)?page-?speed\b[`'"]?\s*[:=]\s*[`'"]?(?:\d+\.\d+[\w.+-]*|<[^>\n]*version[^>\n]*>|WeAmp\.PageSpeed\/|\$\{[^}\n]*version[^}\n]*\})/i;

function walk(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    const p = path.join(dir, name);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (/\.(md|mdx|astro|ts|mjs)$/.test(name)) out.push(p);
  }
  return out;
}

export function findHeaderVersionExamples(text: string): { line: number; text: string }[] {
  const lines = text.split('\n');
  const hits: { line: number; text: string }[] = [];
  lines.forEach((l, i) => {
    if (!VERSION_IN_HEADER.test(l) || LEGACY_VALUE.test(l)) return;
    const near = lines.slice(Math.max(0, i - NEAR), i + NEAR + 1).join('\n');
    if (near.includes(OPT_IN)) return;
    hits.push({ line: i + 1, text: l.trim() });
  });
  return hits;
}

describe('docs never show a version in a PageSpeed header without the opt-in', () => {
  it('flags a version example and accepts the generic value', () => {
    expect(findHeaderVersionExamples('X-Page-Speed: 2.6.0')).toHaveLength(1);
    expect(findHeaderVersionExamples('`X-Mod-Pagespeed: <version>`')).toHaveLength(1);
    expect(findHeaderVersionExamples('X-PageSpeed: WeAmp.PageSpeed/2.1.0')).toHaveLength(1);
    expect(
      findHeaderVersionExamples('text: `X-Page-Speed: ${MODULE_PACKAGE_VERSION}`'),
    ).toHaveLength(1);
    expect(findHeaderVersionExamples('X-Page-Speed: on')).toHaveLength(0);
    // A re-inserted version is flagged even when the file names the opt-in far away.
    const far = 'ExposeVersionInHeaders\n' + '\n'.repeat(10) + 'X-Page-Speed: 2.6.0';
    expect(findHeaderVersionExamples(far)).toHaveLength(1);
    expect(
      findHeaderVersionExamples('set ExposeVersionInHeaders to see\nX-Page-Speed: 2.6.0'),
    ).toHaveLength(0);
    expect(findHeaderVersionExamples('X-PageSpeed: MISS')).toHaveLength(0);
  });

  it('has no unaccompanied example in the authored docs', () => {
    const offenders: string[] = [];
    for (const d of SCAN_DIRS) {
      for (const file of walk(path.join(ROOT, d))) {
        const rel = path.relative(ROOT, file).split(path.sep).join('/');
        if (SKIP.some((re) => re.test(rel) || re.test('/' + rel))) continue;
        const text = readFileSync(file, 'utf8');
        for (const h of findHeaderVersionExamples(text))
          offenders.push(`${rel}:${h.line}: ${h.text}`);
      }
    }
    expect(
      offenders,
      'document the ExposeVersionInHeaders opt-in beside a version example, or show "on"',
    ).toEqual([]);
  });
});

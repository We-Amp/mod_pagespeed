// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// code-untranslated.test.ts — browser translation must not rewrite directive
// names and shell commands. scripts/mark-code-untranslated.mjs sets
// translate="no" on every <pre> and <code> of the built pages (postbuild).
//
//   1. Unit (always runs): the marker tags pre and code, keeps an explicit
//      translate, and leaves comments, scripts, styles and textareas alone.
//   2. Built output (runs after `npm run build`, skips itself otherwise): no
//      page has a <pre> or <code> start tag without translate.

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { markCodeUntranslated } from '../scripts/mark-code-untranslated.mjs';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));

function htmlFiles(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? htmlFiles(p) : p.endsWith('.html') ? [p] : [];
  });
}

describe('markCodeUntranslated (unit)', () => {
  it('marks pre and code, with and without attributes', () => {
    expect(markCodeUntranslated('<p>Use <code>EnableFilters</code>.</p>')).toBe(
      '<p>Use <code translate="no">EnableFilters</code>.</p>',
    );
    expect(
      markCodeUntranslated(
        '<pre class="astro-code" tabindex="0" data-language="sh"><code>x</code></pre>',
      ),
    ).toBe(
      '<pre class="astro-code" tabindex="0" data-language="sh" translate="no"><code translate="no">x</code></pre>',
    );
  });

  it('keeps an explicit translate and tags that only start with pre or code', () => {
    for (const html of [
      '<code translate="yes">x</code>',
      '<pre translate=no>x</pre>',
      '<preview>x</preview>',
      '<codex>x</codex>',
    ]) {
      expect(markCodeUntranslated(html)).toBe(html);
    }
  });

  it('leaves comments, scripts, styles and textareas alone', () => {
    for (const html of [
      '<!-- <code>x</code> -->',
      '<!-- <pre> --!>',
      '<script>const a = "<code>";</script>',
      '<style>/* <pre> */</style>',
      '<textarea><code>x</code></textarea>',
    ]) {
      expect(markCodeUntranslated(html)).toBe(html);
    }
    expect(markCodeUntranslated('<script>"<pre>"</script><code>y</code>')).toBe(
      '<script>"<pre>"</script><code translate="no">y</code>',
    );
  });
});

describe.skipIf(!existsSync(DIST))('built pages (after npm run build)', () => {
  it('every pre and code start tag says translate', () => {
    const offenders = htmlFiles(DIST).filter((f) => {
      const html = readFileSync(f, 'utf8');
      return markCodeUntranslated(html) !== html;
    });
    expect(offenders.map((f) => path.relative(DIST, f))).toEqual([]);
  });
});

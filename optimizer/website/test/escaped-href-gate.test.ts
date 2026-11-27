// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// escaped-href-gate.test.ts: no link may start with a quote. A JSON-style
// escape inside a single-quoted TypeScript string (href=\\"/x/\\") renders as
// href="\"/x/\"", which resolves to a path that does not exist. Two layers:
//
//   1. Source (always runs): no source string under src/data carries the
//      escaped form.
//   2. Built output (runs after `npm run build`, skips itself otherwise, like
//      link-punctuation.test.ts): no built page has an href that starts with a
//      backslash, a quote or an encoded quote.

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = path.join(ROOT, 'dist/client');

function files(dir: string, ext: RegExp): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? files(p, ext) : ext.test(e.name) ? [p] : [];
  });
}

// href=\" or href=\\" in source, and href="\" / href="&quot; / href="%22 as rendered.
const SOURCE_ESCAPE = /href=\\+"/;
const RENDERED_ESCAPE = /href=(?:"(?:\\|&quot;|&#34;|%22)|'(?:\\|&quot;|%22|"))/i;

describe('escaped hrefs', () => {
  it('no source string under src/data escapes the quotes of an href', () => {
    const bad = files(path.join(ROOT, 'src/data'), /\.(ts|mjs)$/).filter((f) =>
      SOURCE_ESCAPE.test(readFileSync(f, 'utf8')),
    );
    expect(bad).toEqual([]);
  });

  it.skipIf(!existsSync(DIST))('no built page has an href that starts with a quote', () => {
    const bad = files(DIST, /\.html$/)
      .filter((f) => RENDERED_ESCAPE.test(readFileSync(f, 'utf8')))
      .map((f) => path.relative(DIST, f));
    expect(bad).toEqual([]);
  });
});

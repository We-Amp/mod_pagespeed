// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// link-punctuation.test.ts — the formatter writes a closing </a> and the
// punctuation after it on separate lines, which renders as "Privacy policy ."
// with the underline running into the gap. scripts/attach-link-punctuation.mjs
// closes that gap in the built pages (postbuild). Two layers:
//
//   1. Unit (always runs): the normalizer joins the pieces and leaves
//      literal blocks alone.
//   2. Built output (runs after `npm run build`, skips itself otherwise, like
//      meta-length-gate.test.ts): no page has </a>, whitespace, then . , ; : ) ! ?

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { attachLinkPunctuation } from '../scripts/attach-link-punctuation.mjs';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));
const GAP = /<\/(?:a|code)>\s+[.,;:)!?]/;

function htmlFiles(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? htmlFiles(p) : p.endsWith('.html') ? [p] : [];
  });
}

describe('attachLinkPunctuation (unit)', () => {
  it('joins a link and the punctuation after it, inside and outside the link', () => {
    const out = attachLinkPunctuation('<p>See <a href="/p/">\n  Privacy policy\n</a>\n.\n</p>');
    expect(out).toBe('<p>See <a href="/p/">\n  Privacy policy</a>.\n</p>');
  });

  it('leaves words after a link and literal blocks alone', () => {
    const html = '<a href="/x/">x</a> and <pre><a>y</a>\n.</pre>';
    expect(attachLinkPunctuation(html)).toBe(html);
  });

  it('joins inline code and the punctuation after it', () => {
    expect(attachLinkPunctuation('<code>--flag</code>\n.')).toBe('<code>--flag</code>.');
  });

  it('leaves pre blocks with attributes, unclosed pre and spaced closers alone', () => {
    for (const html of [
      '<pre class="a b"><a>y</a>\n.</pre>',
      '<pre><a>y</a>\n.',
      '<script><a>y</a>\n.</script ><p>z</p>',
      '<pre><code>x</code>\n.</pre >',
      '<!-- a <style> mention --><a>y</a>.',
    ]) {
      expect(attachLinkPunctuation(html)).toBe(html);
    }
    expect(attachLinkPunctuation('<script>1</script ><a>y</a>\n.')).toBe(
      '<script>1</script ><a>y</a>.',
    );
  });

  it('does not let a tag named in a comment swallow the page', () => {
    expect(attachLinkPunctuation('<!-- <style> --><a>y</a>\n.')).toBe('<!-- <style> --><a>y</a>.');
  });

  it('ends a comment at --!> as well as -->', () => {
    const html = '<!-- <pre> --!><a>y</a>\n.';
    expect(attachLinkPunctuation(html)).toBe('<!-- <pre> --!><a>y</a>.');
    expect(attachLinkPunctuation('<!-- x --!><pre><a>y</a>\n.</pre>')).toBe(
      '<!-- x --!><pre><a>y</a>\n.</pre>',
    );
  });

  it('keeps a non-breaking space before punctuation', () => {
    const html = '<a href="/x/">x</a> .';
    expect(attachLinkPunctuation(html)).toBe(html);
  });

  it('handles a very long whitespace run quickly', () => {
    const html = `<p>${' '.repeat(300_000)}x</p>${' '.repeat(300_000)}<a>y</a>${' '.repeat(300_000)}.`;
    const start = performance.now();
    const out = attachLinkPunctuation(html);
    expect(performance.now() - start).toBeLessThan(500);
    expect(out.endsWith('<a>y</a>.')).toBe(true);
  });
});

describe.skipIf(!existsSync(DIST))('built pages (after npm run build)', () => {
  it('no page separates a link from the punctuation that follows it', () => {
    const offenders = htmlFiles(DIST).filter((f) => GAP.test(readFileSync(f, 'utf8')));
    expect(offenders.map((f) => path.relative(DIST, f))).toEqual([]);
  });
});

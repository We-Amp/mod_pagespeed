// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { renderPostMarkdown } from '../../src/lib/blog-markdown.mjs';

/** A synthetic post around `body`. */
function render(body: string): string {
  return renderPostMarkdown({
    slug: 'synthetic',
    file: 'synthetic.md',
    data: { title: 'Synthetic', description: 'A synthetic post.', date: '2026-01-01' },
    date: '2026-01-01',
    body,
  });
}

describe('renderPostMarkdown on hostile input', () => {
  it('keeps the prose after a self-closing block element', () => {
    const md = render('Before.\n\n<div class="spacer"/>\n\nAfter the spacer.\n\nLast line.');
    expect(md).toContain('Before.');
    expect(md).toContain('After the spacer.');
    expect(md).toContain('Last line.');
  });

  it('keeps text after the close of a comment on the same line', () => {
    const md = render('<!-- marker --> Kept after the comment.\n\nNext.');
    expect(md).toContain('Kept after the comment.');
    expect(md).not.toContain('marker');
    expect(md).toContain('Next.');
  });

  it('ends an unclosed comment at the end of its block, not the file', () => {
    const md = render('<!-- never closed\nstill the comment\n\nProse after the block.');
    expect(md).not.toContain('still the comment');
    expect(md).toContain('Prose after the block.');
  });

  it('closes a fence only on its marker followed by whitespace', () => {
    const body = [
      '````md',
      '```',
      'inner fence line',
      '```',
      '````',
      '',
      '```js',
      'const a = 1;',
      '```js',
      '```',
      '',
      'Outside.',
    ].join('\n');
    const md = render(body);
    expect(md).toContain('````md\n```\ninner fence line\n```\n````\n');
    expect(md).toContain('```js\nconst a = 1;\n```js\n```\n');
    expect(md).toContain('Outside.');
  });

  it('prefers the svg title, then the figcaption, then the aria-label without the interface clause', () => {
    const titled = render(
      '<figure><svg aria-label="x"><title>Title text</title></svg><figcaption>Cap</figcaption></figure>',
    );
    expect(titled).toContain('Figure: Title text');
    const captioned = render(
      '<figure aria-label="x"><svg></svg><figcaption>Cap</figcaption></figure>',
    );
    expect(captioned).toContain('Figure: Cap');
    const labelled = render(
      '<div aria-label="A chart of sites; scrolls horizontally on small screens">\n</div>',
    );
    expect(labelled).toContain('Figure: A chart of sites\n');
    expect(labelled).not.toContain('scrolls');
  });
});

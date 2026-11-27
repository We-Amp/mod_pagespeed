// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// remark-literal-text-directives.test.ts — inline `:word` text in Markdown
// (host:port, CookieName[:RedirectUrl]) renders as written instead of being
// read as an inline directive and dropped (src/lib/remark-literal-text-directives.mjs).
// The configuration reference, its `.md` twin and llms-full.txt all keep the
// tokens.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import { createMarkdownProcessor } from '@astrojs/markdown-remark';
import remarkCustomHeadingId from 'remark-custom-heading-id';
import remarkDirective from 'remark-directive';
import remarkCallouts from '../src/lib/remark-callouts.mjs';
import remarkLiteralTextDirectives from '../src/lib/remark-literal-text-directives.mjs';
import { stripFrontmatter } from './lib/markdown-ids';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const read = (file: string) => readFileSync(resolve(WEBSITE_ROOT, file), 'utf8');

// The same remark plugins as astro.config.mjs.
async function render(markdown: string) {
  const processor = await createMarkdownProcessor({
    remarkPlugins: [
      remarkCustomHeadingId,
      remarkDirective,
      remarkLiteralTextDirectives,
      remarkCallouts,
    ],
  });
  return (await processor.render(markdown)).code;
}

// Text a reader sees: drop every tag, then decode the three entities the
// renderer emits, in one pass so no decoded text is decoded again.
const ENTITIES: Record<string, string> = { '&#x3C;': '<', '&#x26;': '&', '&amp;': '&' };
const visibleText = (html: string) => {
  let text = '';
  let inTag = false;
  for (const ch of html) {
    if (ch === '<') inTag = true;
    else if (ch === '>' && inTag) inTag = false;
    else if (!inTag) text += ch;
  }
  return text.replace(/&#x3C;|&#x26;|&amp;/g, (entity) => ENTITIES[entity]);
};

// Tokens from the generated reference that read like inline directives.
const TOKENS = [
  'CookieName[=Value][:RedirectUrl]',
  'scheme://host[:port]',
  'The host:port/path prefix',
  'host1:port1,host2:port2',
];

describe('inline :word text renders as written', () => {
  it('keeps the colon and the word, in one paragraph', async () => {
    const html = await render(
      'Use host:port, [:port] or Name[:RedirectUrl] and nginx:latest or 4:2:0.\n',
    );
    expect(html).toBe(
      '<p>Use host:port, [:port] or Name[:RedirectUrl] and nginx:latest or 4:2:0.</p>',
    );
  });

  it('keeps labels and attributes as written', async () => {
    const html = await render('a:b[label]{x="1"} c\n');
    expect(visibleText(html)).toBe('a:b[label]{x="1"} c');
  });

  it('leaves callouts working', async () => {
    const html = await render(':::caution[Careful]\nhost:port here\n:::\n');
    expect(html).toContain('<aside class="callout callout-caution">');
    expect(html).toContain('<p class="callout-title">Careful</p>');
    expect(html).toContain('host:port here');
  });

  it('the configuration reference keeps every token, and no empty element', async () => {
    const html = await render(stripFrontmatter(read('src/content/docs/configuration.md')));
    const text = visibleText(html);
    for (const token of TOKENS) expect(text, token).toContain(token);
    expect(html).not.toContain('<div></div>');
  }, 120_000);

  it('the Markdown twin source and llms-full.txt keep every token', () => {
    const twin = read('src/content/docs/configuration.md');
    const llms = read('public/llms-full.txt');
    for (const token of TOKENS) {
      expect(twin, token).toContain(token);
      expect(llms, token).toContain(token);
    }
  });
});

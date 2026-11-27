// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// directive-blocks.test.ts — the render-time grouping of the directive
// reference (src/lib/rehype-directive-blocks.mjs).
//
//   /docs/configuration/   every directive in module-directives.json has its
//                          heading marked with its name, every area heading
//                          is marked, no element is added per directive or
//                          per area, the finder sits above the first area,
//                          and no id or text is gained or lost
//   every other page       rendered exactly as without the plugin

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect, beforeAll } from 'vitest';
import { createMarkdownProcessor } from '@astrojs/markdown-remark';
import remarkCustomHeadingId from 'remark-custom-heading-id';
import remarkDirective from 'remark-directive';
import remarkCallouts from '../../src/lib/remark-callouts.mjs';
import rehypeDirectiveBlocks, { FINDER_LABEL } from '../../src/lib/rehype-directive-blocks.mjs';
import { headingId } from '../../scripts/render-references.mjs';
import { stripFrontmatter } from '../lib/markdown-ids';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const readDoc = (file: string) =>
  stripFrontmatter(readFileSync(resolve(WEBSITE_ROOT, 'src/content/docs', file), 'utf8'));
const { directives, areas } = JSON.parse(
  readFileSync(resolve(WEBSITE_ROOT, 'src/data/reference/module-directives.json'), 'utf8'),
) as { directives: { name: string; area: string }[]; areas: string[] };

async function render(markdown: string, grouped: boolean) {
  const processor = await createMarkdownProcessor({
    // The same remark plugins as astro.config.mjs.
    remarkPlugins: [remarkCustomHeadingId, remarkDirective, remarkCallouts],
    rehypePlugins: grouped ? [rehypeDirectiveBlocks] : [],
  });
  return (await processor.render(markdown)).code;
}

const ids = (html: string) => [...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]);
const visibleText = (html: string) =>
  html
    .replace(/<[^>]+>/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();

describe('the directive reference is grouped at render time', () => {
  let plain = '';
  let grouped = '';
  beforeAll(async () => {
    const source = readDoc('configuration.md');
    [plain, grouped] = await Promise.all([render(source, false), render(source, true)]);
  }, 120_000);

  it('marks every directive heading with its name', () => {
    const marked = [...grouped.matchAll(/<h4 id="([^"]+)" data-directive="([^"]*)"/g)];
    expect(marked.length).toBe(directives.length);
    expect(grouped.match(/data-directive="/g)!.length).toBe(directives.length);
    const byId = new Map(marked.map((m) => [m[1], m[2]]));
    for (const d of directives) {
      const title = d.name === 'ModPagespeed' ? 'pagespeed / ModPagespeed' : d.name;
      expect(byId.get(headingId(d.name)), d.name).toBe(title);
    }
  });

  it('marks every area heading and its list of directives', () => {
    const headings = grouped.match(/<h3 id="[^"]+" data-directive-area=""/g)!;
    const populated = areas.filter((a) => directives.some((d) => d.area === a));
    expect(headings.length).toBe(populated.length);
    expect(grouped.match(/<p data-directive-index="">/g)!.length).toBe(populated.length);
  });

  it('adds no element per directive or per area', () => {
    const tags = (html: string, skip = /^<(span|p)$/) =>
      html.match(/<[a-z][a-z0-9]*/g)!.filter((t) => !skip.test(t)).length;
    // The reference wrapper and the field: a div, a div, a label and an
    // input (the status line is a p). Spans and paragraphs only come from
    // splitting a paragraph the browser would split anyway.
    expect(tags(grouped) - tags(plain)).toBe(4);
    expect(grouped).not.toMatch(/<section|<div data-directive=/);
  });

  it('leaves no loose text between the blocks of the reference', () => {
    // Text right after a closed block element, outside any element: the
    // finder could not hide it. remark-directive reads `:word` in the text as
    // a directive, which splits paragraphs this way.
    const loose = /<\/(?:div|ul|ol|pre|table|p|h[1-6]|blockquote)>\s*[^<\s]/;
    const start = grouped.indexOf('<div data-directive-reference');
    expect(grouped.slice(start)).not.toMatch(loose);
    // The source does have such text, so the wrapping is exercised.
    expect(plain.slice(start)).toMatch(loose);
  });

  it('puts one labelled field above the first area', () => {
    expect(grouped.match(/data-directive-finder/g)!.length).toBe(1);
    expect(grouped).toContain(`<label for="directive-filter">${FINDER_LABEL}</label>`);
    expect(grouped).toMatch(/<input id="directive-filter" type="search"[^>]*>/);
    expect(grouped).toMatch(/<p id="directive-filter-status" role="status"><\/p>/);
    expect(grouped.indexOf('data-directive-finder')).toBeLessThan(
      grouped.indexOf('data-directive-area'),
    );
    expect(grouped.indexOf('data-directive-reference')).toBeLessThan(
      grouped.indexOf('data-directive-finder'),
    );
  });

  it('keeps every id, in order, and adds only the field and its status line', () => {
    const added = new Set(['directive-filter', 'directive-filter-status']);
    expect(ids(grouped).filter((id) => !added.has(id))).toEqual(ids(plain));
  });

  it('keeps the text of the page, adding only the label', () => {
    expect(visibleText(grouped).replace(` ${FINDER_LABEL} `, ' ')).toBe(visibleText(plain));
  });

  it('leaves the rest of the page outside the reference alone', () => {
    const start = grouped.indexOf('<div data-directive-reference');
    expect(start).toBeGreaterThan(0);
    expect(grouped.slice(0, start)).toBe(plain.slice(0, start));
    const tail = plain.slice(plain.indexOf('<!-- generated:end module-directives -->'));
    expect(grouped.endsWith(tail)).toBe(true);
  });
});

describe('pages without the directive reference are untouched', () => {
  for (const file of ['worker-configuration.md', 'filter-selection.md']) {
    it(
      file,
      async () => {
        const source = readDoc(file);
        expect(await render(source, true)).toBe(await render(source, false));
      },
      120_000,
    );
  }
});

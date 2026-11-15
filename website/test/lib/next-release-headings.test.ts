// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { unified } from 'unified';
import remarkParse from 'remark-parse';
import remarkDirective from 'remark-directive';
import remarkRehype from 'remark-rehype';
import { rehypeHeadingIds } from '@astrojs/markdown-remark';
import remarkCallouts from '../../src/lib/remark-callouts.mjs';
import rehypeNextReleaseHeadings from '../../src/lib/rehype-next-release-headings.mjs';
import { tocHeadings } from '../../src/lib/toc-headings';

/** The ids the docs pipeline records for a Markdown body, as the build runs it. */
async function recordedIds(markdown: string): Promise<unknown> {
  const processor = unified()
    .use(remarkParse)
    .use(remarkDirective)
    .use(remarkCallouts)
    .use(remarkRehype)
    .use(rehypeHeadingIds)
    .use(rehypeNextReleaseHeadings);
  const file = { value: markdown, data: {} as Record<string, unknown>, history: ['page.md'] };
  await processor.run(processor.parse(markdown), file as never);
  return (file.data.astro as { frontmatter: Record<string, unknown> }).frontmatter
    .nextReleaseHeadingIds;
}

describe('headings inside next-release markers', () => {
  const body = [
    '## Released',
    '',
    ':::next-release{after="2.8.2"}',
    '## Not yet',
    '',
    '### Detail',
    '',
    'Text.',
    ':::',
    '',
    ':::note',
    '## In a note',
    ':::',
  ].join('\n');

  it('records the ids of the headings inside a marker, and only those', async () => {
    expect(await recordedIds(body)).toEqual(['not-yet', 'detail']);
  });

  it('records an empty list for a page without markers', async () => {
    expect(await recordedIds('## A\n\n## B\n')).toEqual([]);
  });

  it('leaves those headings out of "On this page", keeping the rest', () => {
    const headings = [
      { depth: 2, slug: 'released', text: 'Released' },
      { depth: 2, slug: 'not-yet', text: 'Not yet' },
      { depth: 3, slug: 'detail', text: 'Detail' },
      { depth: 4, slug: 'deep', text: 'Deep' },
      { depth: 2, slug: 'in-a-note', text: 'In a note' },
    ];
    expect(
      tocHeadings(headings, { nextReleaseHeadingIds: ['not-yet', 'detail'] }).map((h) => h.slug),
    ).toEqual(['released', 'in-a-note']);
    expect(tocHeadings(headings, undefined).map((h) => h.slug)).toEqual([
      'released',
      'not-yet',
      'detail',
      'in-a-note',
    ]);
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// meta-length-gate.test.ts — search engines cut a <title> past roughly 60
// characters and a meta description past roughly 155, so a longer value loses
// its last words in the results page. Two layers:
//
//   1. Source (always runs): the frontmatter of the blog, docs and
//      how-it-works collections stays within the limits the content schema in
//      src/content.config.ts enforces, so a regression is caught without a build.
//   2. Built output (runs after `npm run build`, skips itself otherwise, like
//      anchor-gate.test.ts): every indexable page in dist/client, including the
//      .astro pages the schema does not cover, has a title of 60 characters or
//      fewer and a description of 155 or fewer, and no page calls the
//      open-source project "Google's mod_pagespeed".

import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse as parseYaml } from 'yaml';
import { describe, it, expect } from 'vitest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const CONTENT = path.join(WEBSITE_ROOT, 'src/content');
const DIST = path.join(WEBSITE_ROOT, 'dist/client');

const TITLE_MAX = 60;
const DESCRIPTION_MAX = 155;

const length = (s: string) => [...s].length;

function frontmatter(file: string): Record<string, unknown> {
  const text = readFileSync(file, 'utf8');
  const match = text.match(/^---\n([\s\S]*?)\n---/);
  if (!match) throw new Error(`${file}: no frontmatter`);
  return parseYaml(match[1]) as Record<string, unknown>;
}

describe('collection frontmatter lengths (source)', () => {
  for (const collection of ['blog', 'docs', 'how-it-works']) {
    const dir = path.join(CONTENT, collection);
    const files = readdirSync(dir).filter((f) => /\.mdx?$/.test(f));

    it(`${collection}: every title is ${TITLE_MAX} characters or fewer`, () => {
      const over = files
        .map((f) => ({ f, data: frontmatter(path.join(dir, f)) }))
        .flatMap(({ f, data }) =>
          (['title', 'seoTitle'] as const)
            .filter((k) => typeof data[k] === 'string' && length(data[k] as string) > TITLE_MAX)
            .map((k) => `${f} ${k} (${length(data[k] as string)}): ${data[k]}`),
        );
      expect(over).toEqual([]);
    });

    it(`${collection}: every description is ${DESCRIPTION_MAX} characters or fewer`, () => {
      const over = files
        .map((f) => ({ f, data: frontmatter(path.join(dir, f)) }))
        .filter(
          ({ data }) =>
            typeof data.description === 'string' && length(data.description) > DESCRIPTION_MAX,
        )
        .map(({ f, data }) => `${f} (${length(data.description as string)}): ${data.description}`);
      expect(over).toEqual([]);
    });
  }
});

function decode(s: string): string {
  return s
    .replace(/&amp;/g, '&')
    .replace(/&#39;|&#x27;|&apos;/g, "'")
    .replace(/&quot;/g, '"')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>');
}

function builtPages(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    const p = path.join(dir, name);
    if (statSync(p).isDirectory()) {
      // The /1.0/ tree is a frozen, noindex archive; pagefind and _astro hold assets.
      if (['1.0', 'pagefind', '_astro'].includes(name)) continue;
      builtPages(p, out);
    } else if (name === 'index.html') {
      out.push(p);
    }
  }
  return out;
}

const BUILT = existsSync(DIST);
if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[meta-length-gate] built output not found at ${DIST} — skipping the built-HTML checks. ` +
      'Run "npm run build" first to exercise them.',
  );
}

describe.skipIf(!BUILT)('built pages: title and description lengths', () => {
  const pages = BUILT ? builtPages(DIST) : [];
  const indexable = pages
    .map((file) => ({ file, html: readFileSync(file, 'utf8') }))
    .filter(({ html }) => !/<meta name="robots" content="noindex/.test(html));
  const rel = (file: string) => `/${path.relative(DIST, path.dirname(file))}/`.replace('//', '/');

  it('finds the built pages', () => {
    expect(indexable.length).toBeGreaterThan(150);
  });

  it(`every indexable title is ${TITLE_MAX} characters or fewer`, () => {
    const over = indexable
      .map(({ file, html }) => ({
        url: rel(file),
        t: decode(html.match(/<title>([\s\S]*?)<\/title>/)?.[1] ?? ''),
      }))
      .filter(({ t }) => length(t) > TITLE_MAX)
      .map(({ url, t }) => `${url} (${length(t)}): ${t}`);
    expect(over).toEqual([]);
  });

  it(`every indexable description is ${DESCRIPTION_MAX} characters or fewer`, () => {
    const over = indexable
      .map(({ file, html }) => ({
        url: rel(file),
        d: decode(html.match(/<meta name="description" content="([^"]*)"/)?.[1] ?? ''),
      }))
      .filter(({ d }) => length(d) > DESCRIPTION_MAX)
      .map(({ url, d }) => `${url} (${length(d)}): ${d}`);
    expect(over).toEqual([]);
  });

  it('no page calls the open-source project "Google\'s mod_pagespeed"', () => {
    const possessive =
      /Google(?:'|’|&#39;|&#x27;|&rsquo;)s(?:\s|<[^>]*>)*(?:mod_pagespeed|ngx_pagespeed)/;
    const hits = pages
      .filter((file) => possessive.test(readFileSync(file, 'utf8')))
      .map((file) => rel(file));
    expect(hits).toEqual([]);
  });
});

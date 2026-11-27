// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// static-html-pages.test.ts — pages served as hand-written HTML from public/
// sit outside the layout, so they get none of its head tags and the sitemap
// integration never sees them. Two layers:
//
//   1. Source (always runs): every indexable HTML page in public/ (a canonical
//      link and no robots noindex) is listed in STATIC_INDEXABLE_PAGES, and
//      each listed page carries the social tags the layout gives every other
//      page and the site's ` — mod_pagespeed` title suffix.
//   2. Built output (runs after `npm run build`, skips itself otherwise): each
//      listed page is in the sitemap.

import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { STATIC_INDEXABLE_PAGES } from '../src/lib/static-html-pages.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const PUBLIC = path.join(WEBSITE_ROOT, 'public');
const DIST = path.join(WEBSITE_ROOT, 'dist/client');
const SITE = 'https://modpagespeed.com';

function htmlFiles(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const full = path.join(dir, name);
    if (statSync(full).isDirectory()) out.push(...htmlFiles(full));
    else if (name.endsWith('.html')) out.push(full);
  }
  return out;
}

function urlPath(file: string): string {
  const rel = path.relative(PUBLIC, file).split(path.sep).join('/');
  return '/' + rel.replace(/(^|\/)index\.html$/, '$1').replace(/\.html$/, '');
}

const attr = (html: string, re: RegExp) => html.match(re)?.[1];
const meta = (html: string, key: string) =>
  attr(html, new RegExp(`<meta (?:property|name)="${key}" content="([^"]*)"`));

// The /1.0/ archive is noindex through a response header and is out of scope.
const pages = htmlFiles(PUBLIC)
  .filter((f) => !urlPath(f).startsWith('/1.0/'))
  .map((file) => ({ file, url: urlPath(file), html: readFileSync(file, 'utf8') }));

describe('hand-written HTML pages in public/ (source)', () => {
  it('lists every indexable page for the sitemap', () => {
    const indexable = pages
      .filter(({ html }) => /<link rel="canonical"/.test(html))
      .filter(({ html }) => !/<meta name="robots" content="[^"]*noindex/.test(html))
      .map(({ url }) => url)
      .sort();
    expect(indexable).toEqual([...STATIC_INDEXABLE_PAGES].sort());
  });

  for (const url of STATIC_INDEXABLE_PAGES) {
    it(`${url} carries a self canonical and the social tags`, () => {
      const page = pages.find((p) => p.url === url);
      expect(page, `${url} has no file in public/`).toBeDefined();
      const html = page!.html;
      const title = attr(html, /<title>([^<]*)<\/title>/);
      const description = meta(html, 'description');
      const canonical = attr(html, /<link rel="canonical" href="([^"]*)"/);
      expect(canonical).toBe(`${SITE}${url}`);
      expect(title).toMatch(/ — mod_pagespeed$/);
      expect(title).not.toContain(' | ');
      expect(meta(html, 'og:title')).toBe(title);
      expect(meta(html, 'og:description')).toBe(description);
      expect(meta(html, 'og:url')).toBe(canonical);
      expect(meta(html, 'og:image')).toMatch(/^https:\/\/modpagespeed\.com\/og-[^"]+\.png/);
      const image = new URL(meta(html, 'og:image')!).pathname;
      expect(existsSync(path.join(PUBLIC, image)), `${image} exists`).toBe(true);
      expect(meta(html, 'twitter:card')).toBe('summary_large_image');
      expect(meta(html, 'twitter:title')).toBe(title);
      expect(meta(html, 'twitter:description')).toBe(description);
      expect(meta(html, 'twitter:image')).toBe(meta(html, 'og:image'));
    });
  }
});

const BUILT = existsSync(path.join(DIST, 'sitemap-0.xml'));

describe.skipIf(!BUILT)('built sitemap', () => {
  it('lists each hand-written indexable page', () => {
    const sitemap = readFileSync(path.join(DIST, 'sitemap-0.xml'), 'utf8');
    for (const url of STATIC_INDEXABLE_PAGES) {
      expect(sitemap).toContain(`<loc>${SITE}${url}</loc>`);
    }
  });
});

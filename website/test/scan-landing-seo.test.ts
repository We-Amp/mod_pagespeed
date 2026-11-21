// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// scan-landing-seo.test.ts — deliberate goldens for the SEO estate of the two
// scan landing pages, /analyze/ and /ai-readability/. A rework of the scan UI
// must not move their title, description, canonical, H1 or structured data,
// so the values below are pinned as literals (not imported from source
// constants): a source edit cannot silently move the golden.
//
// Runs against the built output in dist/client and skips itself before
// `npm run build`, like meta-length-gate.test.ts and anchor-gate.test.ts.
//
// To change a golden on purpose: edit the literal here in the same PR as the
// content change, and state the reason in the PR description.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse } from 'parse5';
import { describe, it, expect } from 'vitest';

const DIST = path.join(fileURLToPath(new URL('..', import.meta.url)), 'dist/client');
const BUILT = existsSync(DIST);
if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[scan-landing-seo] built output not found at ${DIST} — skipping. Run "npm run build" first.`,
  );
}

interface Golden {
  route: string;
  title: string;
  description: string;
  canonical: string;
  h1: string;
  types: string[]; // distinct JSON-LD @type values, sorted
}

const GOLDENS: Golden[] = [
  {
    route: '/analyze/',
    title: 'Free PageSpeed Insights test, with the fix attached',
    description:
      'Run a free PageSpeed Insights test, no signup. Each failing audit is mapped to the mod_pagespeed filter that fixes it, or labeled when none can.',
    canonical: 'https://modpagespeed.com/analyze/',
    h1: 'PageSpeed Insights, with the fix attached.',
    types: [
      'Answer',
      'BreadcrumbList',
      'FAQPage',
      'ListItem',
      'Offer',
      'Organization',
      'Question',
      'WebApplication',
    ],
  },
  {
    route: '/ai-readability/',
    title: 'AI readability checker: what AI crawlers read on your page',
    description:
      'Free check for one page: how much of it an AI crawler can read without running JavaScript. It does not measure whether AI answers mention you.',
    canonical: 'https://modpagespeed.com/ai-readability/',
    h1: 'See what AI crawlers miss. Optimize the HTML you serve, on your own servers.',
    types: [
      'Answer',
      'BreadcrumbList',
      'FAQPage',
      'ListItem',
      'Offer',
      'Question',
      'WebApplication',
    ],
  },
];

interface Node {
  nodeName: string;
  value?: string;
  attrs?: { name: string; value: string }[];
  childNodes?: Node[];
}

const page = (route: string): Node =>
  parse(readFileSync(path.join(DIST, route, 'index.html'), 'utf8')) as unknown as Node;

function all(node: Node, tag: string, out: Node[] = []): Node[] {
  if (node.nodeName === tag) out.push(node);
  (node.childNodes ?? []).forEach((c) => all(c, tag, out));
  return out;
}

const attr = (n: Node, name: string) => n.attrs?.find((a) => a.name === name)?.value;

const text = (n: Node): string =>
  n.nodeName === '#text' ? (n.value ?? '') : (n.childNodes ?? []).map(text).join('');

function meta(doc: Node, key: 'name' | 'property', value: string): string | undefined {
  const m = all(doc, 'meta').find((n) => attr(n, key) === value);
  return m && attr(m, 'content');
}

function collectTypes(node: unknown, out: Set<string>): void {
  if (Array.isArray(node)) {
    node.forEach((n) => collectTypes(n, out));
  } else if (node && typeof node === 'object') {
    for (const [k, v] of Object.entries(node)) {
      if (k === '@type') [v].flat().forEach((t) => out.add(String(t)));
      else collectTypes(v, out);
    }
  }
}

function jsonLdTypes(doc: Node): string[] {
  const out = new Set<string>();
  for (const s of all(doc, 'script')) {
    if (attr(s, 'type') === 'application/ld+json') collectTypes(JSON.parse(text(s)), out);
  }
  return [...out].sort();
}

describe.skipIf(!BUILT)('scan landing pages: SEO goldens (built output)', () => {
  for (const g of GOLDENS) {
    describe(g.route, () => {
      const doc = BUILT ? page(g.route) : ({ nodeName: '' } as Node);

      it('title', () => {
        const titles = all(doc, 'title');
        expect(titles).toHaveLength(1);
        expect(text(titles[0])).toBe(g.title);
      });

      it('meta description', () => {
        expect(meta(doc, 'name', 'description')).toBe(g.description);
      });

      it('canonical, without a query string', () => {
        const links = all(doc, 'link').filter((n) => attr(n, 'rel') === 'canonical');
        expect(links).toHaveLength(1);
        expect(attr(links[0], 'href')).toBe(g.canonical);
        expect(g.canonical).not.toContain('?');
      });

      it('og:title and twitter:title match the title', () => {
        expect(meta(doc, 'property', 'og:title')).toBe(g.title);
        expect(meta(doc, 'name', 'twitter:title')).toBe(g.title);
      });

      it('exactly one h1, with the golden text', () => {
        const h1s = all(doc, 'h1');
        expect(h1s).toHaveLength(1);
        expect(text(h1s[0]).trim()).toBe(g.h1);
      });

      it('JSON-LD @type list', () => {
        expect(jsonLdTypes(doc)).toEqual(g.types);
      });
    });
  }

  it('/analyze/ keeps its FAQPage structured data', () => {
    expect(jsonLdTypes(page('/analyze/'))).toContain('FAQPage');
  });

  it('/pagespeed-insights/ still links to /analyze/ from both CTAs', () => {
    const anchors = all(page('/pagespeed-insights/'), 'a');
    for (const event of ['cta_psi_analyze_hero', 'cta_psi_analyze']) {
      const a = anchors.find((n) => attr(n, 'data-umami-event') === event);
      expect(a && attr(a, 'href')).toBe('/analyze/');
    }
  });
});

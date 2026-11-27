// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// examples-jsonld.test.ts — an indexable example page's TechArticle names the
// product it demonstrates. That reference must be the site-wide product node
// (its `@id`, the current release as `softwareVersion`), not a hand-written
// node with a retired version, and author and publisher must reference the
// site-wide organization node. Runs after `npm run build` and skips itself
// otherwise (like anchor-gate.test.ts).

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse } from 'parse5';
import { describe, it, expect } from 'vitest';
import { ORGANIZATION_ID } from '../src/data/organization';
import { releaseManifest } from './lib/release-manifest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = path.join(WEBSITE_ROOT, 'dist/client');
const EXAMPLES = path.join(DIST, 'examples');
const PRODUCT_ID = 'https://modpagespeed.com/#software';

const BUILT = existsSync(EXAMPLES);
if (!BUILT) {
  console.warn(
    `[examples-jsonld] built output not found at ${EXAMPLES} — skipping. Run \`npm run build\` first.`,
  );
}

type Node = Record<string, unknown>;

type HtmlNode = {
  nodeName: string;
  value?: string;
  attrs?: { name: string; value: string }[];
  childNodes?: HtmlNode[];
};

function* scripts(node: HtmlNode): Generator<HtmlNode> {
  for (const child of node.childNodes ?? []) {
    if (child.nodeName === 'script') yield child;
    yield* scripts(child);
  }
}

// The page's JSON-LD blocks, read with an HTML parser.
function jsonLdNodes(html: string): Node[] {
  return [...scripts(parse(html) as HtmlNode)]
    .filter((s) => s.attrs?.some((a) => a.name === 'type' && a.value === 'application/ld+json'))
    .map(
      (s) => JSON.parse((s.childNodes ?? []).map((c) => c.value ?? '').join('')) as Node | Node[],
    )
    .flatMap((n) => (Array.isArray(n) ? n : [n]));
}

describe.skipIf(!BUILT)('example pages: TechArticle structured data', () => {
  const articles = BUILT
    ? readdirSync(EXAMPLES, { withFileTypes: true })
        .filter((d) => d.isDirectory() && existsSync(path.join(EXAMPLES, d.name, 'index.html')))
        .flatMap((d) =>
          jsonLdNodes(readFileSync(path.join(EXAMPLES, d.name, 'index.html'), 'utf8'))
            .filter((n) => n['@type'] === 'TechArticle')
            .map((article) => ({ slug: d.name, article })),
        )
    : [];

  it('finds indexable example pages with a TechArticle', () => {
    expect(articles.length).toBeGreaterThan(0);
  });

  it('the product node they reference is on the home page', () => {
    const home = jsonLdNodes(readFileSync(path.join(DIST, 'index.html'), 'utf8'));
    expect(home.some((n) => n['@id'] === PRODUCT_ID)).toBe(true);
  });

  for (const { slug, article } of articles) {
    it(`${slug}: about is the product reference at the current release`, () => {
      const about = article.about as Node;
      expect(about['@id']).toBe(PRODUCT_ID);
      expect(about.softwareVersion).toBe(releaseManifest.release.semver);
      expect(about).not.toHaveProperty('version');
      expect((article.author as Node)['@id']).toBe(ORGANIZATION_ID);
      expect((article.publisher as Node)['@id']).toBe(ORGANIZATION_ID);
    });
  }
});

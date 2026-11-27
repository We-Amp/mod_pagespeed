// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// configuration-pages.test.ts — the generated reference pages are complete
// and current.
//
//   /docs/configuration/        every directive in module-directives.json has
//                               its own block (heading with the directive's id)
//   /docs/worker-configuration/ every worker flag and every thin-module
//                               directive has its own block
//   both                        the marked regions equal a fresh render
//                               (scripts/render-references.mjs), so data or
//                               notes changed without `npm run gen:references`
//                               — or a hand edit inside a region — fails here
//   both                        every in-page fragment link resolves, and the
//                               deep-link ids external pages depend on exist

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import { idsInSource, stripFrontmatter, fragmentLinks } from '../lib/markdown-ids';
import {
  renderRegions,
  renderPage,
  headingId,
  prose,
  PAGE_REGIONS,
} from '../../scripts/render-references.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const DOCS_DIR = resolve(WEBSITE_ROOT, 'src/content/docs');
const REFERENCE_DIR = resolve(WEBSITE_ROOT, 'src/data/reference');

const readJson = (name: string) => JSON.parse(readFileSync(resolve(REFERENCE_DIR, name), 'utf8'));
const readDoc = (slug: string) => {
  // A cross-page link may target an .mdx page (http-api, deployment, ...).
  const md = resolve(DOCS_DIR, `${slug}.md`);
  const file = existsSync(md) ? md : resolve(DOCS_DIR, `${slug}.mdx`);
  return stripFrontmatter(readFileSync(file, 'utf8'));
};

const configuration = readDoc('configuration');
const worker = readDoc('worker-configuration');
const configurationIds = idsInSource(configuration);
const workerIds = idsInSource(worker);

describe('/docs/configuration/ documents every module directive', () => {
  const { directives } = readJson('module-directives.json');

  it('has one block per directive, with the directive name as the heading id', () => {
    for (const d of directives) {
      expect(configurationIds.has(headingId(d.name)), `id for ${d.name}`).toBe(true);
      const title = d.name === 'ModPagespeed' ? 'pagespeed / ModPagespeed' : d.name;
      expect(configuration).toContain(`#### ${title} {#${headingId(d.name)}}`);
    }
  });

  it('keeps the deep-link ids the redirect maps and other pages point at', () => {
    for (const id of [
      'native-module-configuration-directives',
      'configuration-directives',
      'location-specific-configuration',
      'virtual-hosts',
      'optimization-threads',
      'max-url-segments',
      'modpagespeed',
      'pagespeed_disallow',
    ]) {
      expect(configurationIds.has(id), id).toBe(true);
    }
  });
});

describe('/docs/worker-configuration/ documents every flag and thin-module directive', () => {
  const { flags } = readJson('worker-flags.json');
  const { directives } = readJson('thin-module-directives.json');

  it('has one block per worker flag', () => {
    for (const f of flags) {
      expect(worker).toContain(`#### ${f.name} {#${headingId(f.name)}}`);
    }
  });

  it('has one block per thin-module directive', () => {
    for (const d of directives) {
      expect(worker).toContain(`#### ${d.name} {#${headingId(d.name)}}`);
    }
  });

  it('keeps the deep-link ids other pages point at', () => {
    for (const id of [
      'cache-directory-generation-check',
      'sizing-the-cache',
      'shared-configuration-file',
      'which-nginx-version',
      'image-pipeline',
      'image-dimensions',
      'lazy-load-images',
      'lcp-preload',
      'preconnect-injection',
      'critical-css',
      'async-css',
      'css-import-flattening',
      'script-deferral',
      'css-minification',
      'js-minification',
      'cache-extension',
      'html-caching',
    ]) {
      expect(workerIds.has(id), id).toBe(true);
    }
  });
});

describe('the generated regions are current', () => {
  const regions = renderRegions();
  for (const file of Object.keys(PAGE_REGIONS)) {
    it(`${file} equals a fresh render (run \`npm run gen:references\` otherwise)`, () => {
      const { current, out } = renderPage(file, regions);
      expect(current).toBe(out);
    });
  }
});

// Markdown shows a code span verbatim, so an HTML character reference inside
// one appears on the page as typed (`&lt;Directory&gt;` instead of
// `<Directory>`). The renderer escapes markup only outside code spans.
describe('generated code spans carry no HTML character references', () => {
  const regions = renderRegions();
  for (const [name, text] of Object.entries(regions)) {
    it(`${name}`, () => {
      const spans = [...text.matchAll(/`[^`\n]*`/g)].map((m) => m[0]);
      expect(spans.length).toBeGreaterThan(0);
      const bad = spans.filter((s) => /&(?:#\d+|#x[0-9a-f]+|[a-z]+);/i.test(s));
      expect(bad).toEqual([]);
    });
  }

  it('prose() keeps markup inside code spans literal and escapes it outside', () => {
    expect(prose('Apache `<Directory>` and <b> * `a*b`')).toBe(
      'Apache `<Directory>` and &lt;b&gt; \\* `a*b`',
    );
  });

  it('prose() escapes a backslash outside code spans so it is shown, not consumed', () => {
    expect(prose('C:\\inetpub\\* and `C:\\x`')).toBe('C:\\\\inetpub\\\\\\* and `C:\\x`');
  });
});

describe('in-page fragment links resolve', () => {
  const pages: Record<string, { body: string; ids: Set<string> }> = {
    configuration: { body: configuration, ids: configurationIds },
    'worker-configuration': { body: worker, ids: workerIds },
  };
  for (const [slug, { body, ids }] of Object.entries(pages)) {
    it(`every #fragment on ${slug} exists`, () => {
      const missing = fragmentLinks(body)
        .filter((l) => l.slug === null || l.slug === slug)
        .filter((l) => !ids.has(l.fragment))
        .map((l) => l.href);
      expect([...new Set(missing)]).toEqual([]);
    });
    it(`every cross-page /docs/…/#fragment on ${slug} exists`, () => {
      const missing: string[] = [];
      for (const l of fragmentLinks(body)) {
        if (l.slug === null || l.slug === slug) continue;
        const target = pages[l.slug]?.ids ?? idsInSource(readDoc(l.slug));
        if (!target.has(l.fragment)) missing.push(l.href);
      }
      expect([...new Set(missing)]).toEqual([]);
    });
  }
});

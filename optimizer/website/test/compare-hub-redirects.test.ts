// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The /vs/ comparison pages merged into the /alternatives/ hub. Every retired
// URL must 301 once, straight to a page that exists, and nothing in the source
// may still link to a retired URL.

import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const config = readFileSync(path.join(ROOT, 'astro.config.mjs'), 'utf8');

const redirects = new Map<string, string>(
  [...config.matchAll(/^\s*'(\/[^']*)':\s*'(\/[^']*)',/gm)].map((m) => [m[1], m[2]]),
);

const RETIRED_SLUGS = [
  'cloudflare-apo',
  'cloudinary',
  'imgix',
  'imgproxy',
  'nitropack',
  'shortpixel',
  'thumbor',
  'wp-rocket',
];

function pageExists(url: string): boolean {
  const rel = url.replace(/^\/|\/$/g, '');
  const base = path.join(ROOT, 'src/pages', rel);
  return (
    existsSync(`${base}.astro`) ||
    existsSync(path.join(base, 'index.astro')) ||
    existsSync(`${base}.md`) ||
    existsSync(`${base}.mdx`)
  );
}

function walk(dir: string): string[] {
  return readdirSync(dir).flatMap((n) => {
    const p = path.join(dir, n);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
}

describe('/vs/ merged into the /alternatives/ hub', () => {
  it('redirects the hub and every comparison page once, page to page', () => {
    expect(redirects.get('/vs/')).toBe('/alternatives/');
    for (const slug of RETIRED_SLUGS) {
      expect(redirects.get(`/vs/${slug}/`), slug).toBe(`/alternatives/${slug}/`);
    }
    expect(redirects.get('/vs/getpagespeed/')).toBe('/alternatives/ngx-pagespeed/');
  });

  it('never redirects to a redirect, and every target page exists', () => {
    for (const [from, to] of redirects) {
      if (!from.startsWith('/vs/')) continue;
      expect(redirects.has(to), `${from} -> ${to} is a chain`).toBe(false);
      expect(pageExists(to), `${to} has no page`).toBe(true);
    }
  });

  it('keeps no page under /vs/', () => {
    expect(existsSync(path.join(ROOT, 'src/pages/vs'))).toBe(false);
  });

  it('has no internal link to a retired /vs/ URL', () => {
    const offenders: string[] = [];
    for (const f of walk(path.join(ROOT, 'src'))) {
      if (!/\.(astro|md|mdx|ts|mjs|json)$/.test(f)) continue;
      const text = readFileSync(f, 'utf8');
      if (/["'(`]\/vs\/|modpagespeed\.com\/vs\//.test(text)) offenders.push(path.relative(ROOT, f));
    }
    expect(offenders).toEqual([]);
  });
});

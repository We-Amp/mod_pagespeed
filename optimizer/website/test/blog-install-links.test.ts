// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// blog-install-links.test.ts: the install links rendered below the fix posts
// name real, published posts and point at card ids that exist on /download/.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { BLOG_INSTALL_PLATFORMS, INSTALL_PLATFORM_LINKS } from '../src/data/blog-install-links';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const download = readFileSync(path.join(root, 'src/pages/download.astro'), 'utf8');

describe('blog install links', () => {
  it.each(Object.keys(BLOG_INSTALL_PLATFORMS))('%s is a published post', (slug) => {
    const file = path.join(root, 'src/content/blog', `${slug}.md`);
    expect(existsSync(file)).toBe(true);
    expect(readFileSync(file, 'utf8')).not.toMatch(/^draft:\s*true/m);
  });

  it.each(Object.entries(INSTALL_PLATFORM_LINKS))('%s points at a /download/ card', (id, link) => {
    expect(link.href).toBe(`/download/#${id}`);
    expect(download).toContain(`id: '${id}'`);
    expect(download).toContain('id={integration.id}');
  });

  it('a post does not mix modules and middleware', () => {
    for (const platforms of Object.values(BLOG_INSTALL_PLATFORMS)) {
      const kinds = new Set(platforms.map((p) => INSTALL_PLATFORM_LINKS[p].kind));
      expect(kinds.size).toBe(1);
    }
  });

  it('each post lists a platform at most once', () => {
    for (const platforms of Object.values(BLOG_INSTALL_PLATFORMS)) {
      expect(new Set(platforms).size).toBe(platforms.length);
    }
  });
});

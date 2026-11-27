// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// /api/product.json must say the same thing as the copy: there is no private
// package repository and no hardened-builds offer; the software and its signed
// packages are the same for everyone.

import { readFileSync } from 'node:fs';
import { describe, expect, it, vi } from 'vitest';
import { MODULE_PACKAGE_VERSION } from '../src/data/product-facts.mjs';
import { GET as productJsonGet } from '../src/pages/api/product.json.ts';
import { releaseManifest } from './lib/release-manifest';

// product.json.ts reads the release manifest through astro:content, which only
// exists inside an Astro build; stand in the real manifest, read from disk.
vi.mock('../src/lib/release', async () => {
  const { releaseManifest } = await import('./lib/release-manifest');
  return { getRelease: async () => releaseManifest };
});

describe('/api/product.json private repository status', () => {
  it('offers no private repository and no hardened builds', async () => {
    const res = (productJsonGet as (ctx?: unknown) => Response)({});
    const data = JSON.parse(await res.text());
    expect(data.artifacts.hardened_builds).toBe('not-offered');
    expect(data.artifacts.hardened_pricing).toBe('not-offered');
    expect(data.artifacts.private_repository).toBe('not-offered');
    for (const tier of data.support.tiers) {
      expect(tier.kind).toBe('support');
      expect(tier.hardened_builds).toBe(false);
    }
    const priority = data.support.tiers.find((t: { id: string }) => t.id === 'priority');
    expect(priority.note).not.toMatch(/private package repository|being set up/);
    const ent = data.support.tiers.find((t: { id: string }) => t.id === 'enterprise');
    expect(ent.includes.join(' ')).toContain('Custom build targets');
  });

  it('carries a release block with the line, version and module package version', async () => {
    const res = (productJsonGet as (ctx?: unknown) => Response)({});
    const data = JSON.parse(await res.text());
    expect(data.release).toEqual({
      line: '2.1',
      version: releaseManifest.release.semver,
      tag: releaseManifest.release.tag,
      released_on: releaseManifest.release.released_on,
      module_package_version: MODULE_PACKAGE_VERSION,
    });
  });
});

describe('release manifest matches the published release notes', () => {
  it('names the manifest version and release date in the release notes page', () => {
    const [major, minor] = releaseManifest.release.semver.split('.');
    const notes = readFileSync(
      new URL(`../src/content/docs/release-notes-${major}-${minor}.mdx`, import.meta.url),
      'utf8',
    );
    expect(notes).toContain(`mod_pagespeed ${releaseManifest.release.semver}`);
    expect(notes).toContain(String(releaseManifest.release.released_on));
  });
});

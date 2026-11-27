// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { execFileSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest';
import {
  latestDate,
  routeFamilyLastmods,
  routeForPageFile,
  staticPageLastmods,
} from './page-lastmod.mjs';

describe('routeForPageFile', () => {
  it('maps static .astro pages to their trailing-slash URL', () => {
    expect(routeForPageFile('index.astro')).toBe('/');
    expect(routeForPageFile('pricing.astro')).toBe('/pricing/');
    expect(routeForPageFile('alternatives/index.astro')).toBe('/alternatives/');
    expect(routeForPageFile('download/apt-yum.astro')).toBe('/download/apt-yum/');
  });

  it('skips dynamic routes and endpoints', () => {
    expect(routeForPageFile('examples/[slug].astro')).toBeNull();
    expect(routeForPageFile('docs/[slug].md.ts')).toBeNull();
    expect(routeForPageFile('api/product.json.ts')).toBeNull();
  });
});

describe('latestDate', () => {
  it('picks the newest known date and ignores unknowns', () => {
    expect(latestDate(['2026-01-02', null, '2026-03-04'])).toBe('2026-03-04');
    expect(latestDate([null, null])).toBeNull();
  });
});

// A throwaway repository with two commits on fixed dates, plus a full and a
// depth-1 clone of it: the depth-1 clone is what a default CI checkout looks like.
const root = mkdtempSync(join(tmpdir(), 'page-lastmod-'));
const origin = join(root, 'origin');
const full = join(root, 'full');
const shallow = join(root, 'shallow');

function git(cwd: string, args: string[], date?: string) {
  execFileSync('git', args, {
    cwd,
    stdio: 'ignore',
    env: {
      ...process.env,
      GIT_AUTHOR_NAME: 'test',
      GIT_AUTHOR_EMAIL: 'test@example.invalid',
      GIT_COMMITTER_NAME: 'test',
      GIT_COMMITTER_EMAIL: 'test@example.invalid',
      ...(date && { GIT_AUTHOR_DATE: date, GIT_COMMITTER_DATE: date }),
    },
  });
}

function write(base: string, rel: string) {
  mkdirSync(join(base, rel, '..'), { recursive: true });
  writeFileSync(join(base, rel), rel);
}

beforeAll(() => {
  mkdirSync(origin);
  git(origin, ['init', '-q']);
  write(origin, 'src/pages/pricing.astro');
  write(origin, 'src/pages/index.astro');
  write(origin, 'src/pages/examples/[slug].astro');
  git(origin, ['add', '.']);
  git(origin, ['commit', '-q', '-m', 'one'], '2025-03-01T12:00:00Z');
  write(origin, 'src/components/home/Hero.astro');
  write(origin, 'src/data/examples-data.json');
  git(origin, ['add', '.']);
  git(origin, ['commit', '-q', '-m', 'two'], '2025-06-15T12:00:00Z');
  git(root, ['clone', '-q', `file://${origin}`, full]);
  git(root, ['clone', '-q', '--depth', '1', `file://${origin}`, shallow]);
});

afterAll(() => rmSync(root, { recursive: true, force: true }));

// git-date caches whether history is usable once per process, so each clone
// gets a fresh copy of the module.
async function freshGitLastModified() {
  vi.resetModules();
  return (await import('./git-date')).gitLastModified;
}

describe('sitemap dates for .astro pages', () => {
  it('dates each page from its last commit, home from its sections too', async () => {
    const dateOf = await freshGitLastModified();
    const map = staticPageLastmods(full, dateOf);
    expect(map.get('/pricing/')).toBe('2025-03-01T00:00:00.000Z');
    expect(map.get('/')).toBe('2025-06-15T00:00:00.000Z');
    expect(map.has('/examples/[slug]/')).toBe(false);
    expect(routeFamilyLastmods(full, dateOf)).toContainEqual({
      prefix: '/examples/',
      iso: '2025-06-15T00:00:00.000Z',
    });
  });

  it('omits the date in a shallow clone instead of using the checkout date', async () => {
    const dateOf = await freshGitLastModified();
    expect(staticPageLastmods(shallow, dateOf).size).toBe(0);
    expect(routeFamilyLastmods(shallow, dateOf)).toEqual([]);
  });

  it('omits the date for a page with no history', () => {
    const map = staticPageLastmods(full, () => null);
    expect(map.size).toBe(0);
  });
});

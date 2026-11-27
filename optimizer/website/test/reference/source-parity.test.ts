// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// source-parity.test.ts — the in-tree half of the reference drift gate.
//
// src/data/reference/worker-flags.json and thin-module-directives.json are
// generated from this repository's own sources (src/worker/main.cc usage
// text, src/nginx/ngx_pagespeed_module.cc command table). This test parses
// those sources again, with the same parsers the generator uses, and demands
// the committed JSON equal the parse — so a flag added to or removed from the
// worker without `npm run gen:references` turns CI red here, and the
// reference pages can never list a flag the binary does not accept.
//
// The module half (module-directives.json, filters.json, generated from the
// public mod_pagespeed tree at the tag in source-pin.json) needs that tree
// and is checked by the website-reference-drift job in
// .github/workflows/website.yml; here only its pin and provenance are checked.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import {
  parseWorkerUsage,
  parseThinModuleDirectives,
} from '../../scripts/lib/reference-parsers.mjs';
import { releaseManifest } from '../lib/release-manifest';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const REPO_ROOT = resolve(WEBSITE_ROOT, '..');
const REFERENCE_DIR = resolve(WEBSITE_ROOT, 'src/data/reference');

const readJson = (name: string) => JSON.parse(readFileSync(resolve(REFERENCE_DIR, name), 'utf8'));

describe('worker-flags.json equals a fresh parse of src/worker/main.cc', () => {
  const committed = readJson('worker-flags.json');
  const parsed = parseWorkerUsage(readFileSync(resolve(REPO_ROOT, 'src/worker/main.cc'), 'utf8'));

  it('names the source file', () => {
    expect(committed.source.path).toBe('src/worker/main.cc');
  });

  it('parses a non-trivial number of flags', () => {
    expect(parsed.length).toBeGreaterThan(100);
  });

  it('matches flag for flag (run `npm run gen:references` after changing the usage text)', () => {
    expect(committed.flags).toEqual(parsed);
  });

  it('has no duplicate flag names', () => {
    const names = parsed.map((f: { name: string }) => f.name);
    expect(new Set(names).size).toBe(names.length);
  });
});

describe('thin-module-directives.json equals a fresh parse of src/nginx/ngx_pagespeed_module.cc', () => {
  const committed = readJson('thin-module-directives.json');
  const parsed = parseThinModuleDirectives(
    readFileSync(resolve(REPO_ROOT, 'src/nginx/ngx_pagespeed_module.cc'), 'utf8'),
  );

  it('names the source file', () => {
    expect(committed.source.path).toBe('src/nginx/ngx_pagespeed_module.cc');
  });

  it('lists the 17 thin-module directives', () => {
    expect(parsed.length).toBe(17);
  });

  it('matches directive for directive', () => {
    expect(committed.directives).toEqual(parsed);
  });

  it('derives a default for every directive that stores a value', () => {
    for (const d of parsed) {
      if (d.name === 'pagespeed_disallow') continue; // a repeatable pattern list, no default
      expect(d.default, `${d.name} default`).not.toBeNull();
    }
  });
});

describe('the module data carries its provenance', () => {
  const pin = readJson('source-pin.json');
  const directives = readJson('module-directives.json');
  const filters = readJson('filters.json');

  it('source-pin.json names a v2.* tag of the public mod_pagespeed repository', () => {
    expect(pin.mod_pagespeed).toMatch(/^v2\.\d+\.\d+$/);
  });

  // The reference says it lists every directive the shipped module accepts,
  // which is only true while the pin is the current release. Bumping
  // release.semver in the release manifest (src/content/releases-2.1/release.yaml) without
  // regenerating the reference (`npm run gen:references` against the new tag)
  // turns this red.
  it('source-pin.json is not behind the current release in the release manifest', () => {
    // MAJOR.MINOR.PATCH only; a pre-release suffix (2.6.0-rc.1) is dropped.
    const version = (v: string) =>
      v
        .replace(/^v/, '')
        .replace(/[-+].*$/, '')
        .split('.')
        .map(Number);
    const pinned = version(pin.mod_pagespeed);
    const current = version(releaseManifest.release.semver);
    const cmp = pinned.map((n, i) => n - current[i]).find((d) => d !== 0) ?? 0;
    expect(
      cmp,
      `source-pin.json pins ${pin.mod_pagespeed}, but the current release is ${releaseManifest.release.semver}; ` +
        'regenerate the reference against that release (npm run gen:references) and commit the result',
    ).toBeGreaterThanOrEqual(0);
  });

  it('module-directives.json and filters.json were generated from that tag', () => {
    expect(directives.source.repo).toBe('https://github.com/We-Amp/mod_pagespeed');
    expect(directives.source.tag).toBe(pin.mod_pagespeed);
    expect(filters.source.tag).toBe(pin.mod_pagespeed);
    expect(directives.source.commit).toMatch(/^[0-9a-f]{40}$/);
  });

  it('every directive has a name, a kind, an area and at least one platform', () => {
    for (const d of directives.directives) {
      expect(d.name).toMatch(/^[A-Za-z]/);
      expect(['option', 'directive', 'deprecated', 'renamed']).toContain(d.kind);
      expect(directives.areas).toContain(d.area);
      expect(d.platforms.length).toBeGreaterThan(0);
    }
  });

  it('lists every filter name once and every alias with members', () => {
    const names = filters.filters.map((f: { name: string }) => f.name);
    expect(new Set(names).size).toBe(names.length);
    expect(names.length).toBeGreaterThanOrEqual(96);
    for (const a of filters.aliases) expect(a.members.length).toBeGreaterThan(0);
  });
});

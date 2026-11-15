// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// next-release-markers.test.ts — the `:::next-release{after="X.Y.Z"}` markers
// expire. A marker wraps docs text about behaviour that is merged but not in a
// release yet; `after` is the last released version when it was written. Once
// a newer version is released, the text describes the released product and
// the marker has to go, so this gate fails and names each one. The release's
// own site update (the change that bumps the version below) is where it fires.
//
// "Released" is the newer of the release manifest's semver
// (src/content/releases-2.1/release.yaml) and MODULE_PACKAGE_VERSION
// (src/data/product-facts.mjs): a full release moves both, a module-only
// release moves only the module packages, and either one means the merged
// behaviour has shipped.
//
// It also covers the generated references: src/data/reference/next-release.json
// marks entries render-references.mjs wraps in the same marker.

import { readdirSync, readFileSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { MODULE_PACKAGE_VERSION } from '../src/data/product-facts.mjs';
import { nextReleaseMarks } from '../scripts/render-references.mjs';
import { releaseManifest } from './lib/release-manifest';
import { compareVersions } from './lib/version-rules';

const websiteRoot = fileURLToPath(new URL('..', import.meta.url));
const SCAN_DIRS = ['src/content', 'src/data/reference/notes'];
const NEXT_RELEASE_JSON = 'src/data/reference/next-release.json';
const SEMVER = /^\d+\.\d+\.\d+$/;

const releasedVersion = [releaseManifest.release.semver, MODULE_PACKAGE_VERSION].sort(
  compareVersions,
)[1];

interface Marker {
  where: string;
  after: string | null;
}

/** The next-release markers of one Markdown source, outside fenced code. */
function findMarkers(source: string, file: string): Marker[] {
  const out: Marker[] = [];
  let fence: string | null = null;
  source.split('\n').forEach((line, i) => {
    const f = line.match(/^\s*(`{3,}|~{3,})/);
    if (f) {
      if (!fence) fence = f[1];
      else if (f[1][0] === fence[0] && f[1].length >= fence.length) fence = null;
      return;
    }
    if (fence) return;
    // Nested markers count too: inside a blockquote (`> `) or a list item
    // (indented), and with a longer `::::` fence.
    const m = line.match(/^(?:\s*>)*\s*:{3,}next-release(?:\[[^\]]*\])?(\{[^}]*\})?\s*$/);
    if (!m) return;
    const after = m[1]?.match(/\bafter="([^"]*)"/)?.[1] ?? null;
    out.push({ where: `${file}:${i + 1}`, after });
  });
  return out;
}

/** The problems a set of markers has against the released version. */
function markerProblems(markers: Marker[], released: string): string[] {
  const problems: string[] = [];
  for (const { where, after } of markers) {
    if (after === null || after === '') {
      problems.push(
        `${where}: a next-release marker without after="X.Y.Z" (the last released version, ${released} today)`,
      );
    } else if (!SEMVER.test(after)) {
      problems.push(`${where}: after="${after}" is not an X.Y.Z version`);
    } else if (compareVersions(after, released) < 0) {
      problems.push(
        `${where}: after="${after}", but ${released} is released, so this text describes the released product. ` +
          'Remove the marker and keep its content (unwrap it); for a next-release.json entry, delete the entry and run `node scripts/render-references.mjs`.',
      );
    } else if (compareVersions(after, released) > 0) {
      problems.push(
        `${where}: after="${after}" is newer than the released ${released}; use the last released version`,
      );
    }
  }
  return problems;
}

function markdownFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...markdownFiles(full));
    else if (/\.mdx?$/.test(entry.name)) out.push(full);
  }
  return out;
}

function allMarkers(): Marker[] {
  const markers: Marker[] = [];
  for (const dir of SCAN_DIRS) {
    for (const file of markdownFiles(join(websiteRoot, dir))) {
      markers.push(...findMarkers(readFileSync(file, 'utf8'), relative(websiteRoot, file)));
    }
  }
  const json = JSON.parse(readFileSync(join(websiteRoot, NEXT_RELEASE_JSON), 'utf8')) as Record<
    string,
    unknown
  >;
  for (const [region, entries] of Object.entries(json)) {
    if (region === '_comment') continue;
    for (const [name, after] of Object.entries(entries as Record<string, unknown>)) {
      markers.push({
        where: `${NEXT_RELEASE_JSON} ${region}.${name}`,
        after: typeof after === 'string' ? after : null,
      });
    }
  }
  return markers;
}

describe('next-release markers', () => {
  it('none has expired, and each names the last released version', () => {
    expect(markerProblems(allMarkers(), releasedVersion)).toEqual([]);
  });
});

describe('the marker rules', () => {
  const src = [
    'Text.',
    '',
    ':::next-release{after="2.8.2"}',
    'New.',
    ':::',
    '',
    ':::next-release[Coming soon]{after="2.8.1"}',
    ':::',
    ':::next-release',
    ':::',
    '> ::::next-release{after="2.8.0"}',
    '  :::next-release[x]{after="2.8.2"}',
    '```md',
    ':::next-release',
    '```',
  ].join('\n');
  const markers = findMarkers(src, 'page.md');

  it('finds markers with and without a label or after, nested or with a longer fence, not inside fenced code', () => {
    expect(markers).toEqual([
      { where: 'page.md:3', after: '2.8.2' },
      { where: 'page.md:7', after: '2.8.1' },
      { where: 'page.md:9', after: null },
      { where: 'page.md:11', after: '2.8.0' },
      { where: 'page.md:12', after: '2.8.2' },
    ]);
  });

  it('passes a marker on the released version', () => {
    expect(markerProblems([markers[0]], '2.8.2')).toEqual([]);
  });

  it('fails a marker once a newer version is released, telling the author to unwrap it', () => {
    const [problem] = markerProblems([markers[0]], '2.9.0');
    expect(problem).toMatch(/^page\.md:3: after="2\.8\.2", but 2\.9\.0 is released/);
    expect(problem).toMatch(/Remove the marker and keep its content/);
  });

  it('fails a marker without after, or with a version that is not released yet', () => {
    expect(markerProblems([markers[2]], '2.8.2')[0]).toMatch(/without after=/);
    expect(markerProblems([{ where: 'x', after: '2.9.0' }], '2.8.2')[0]).toMatch(/newer than/);
    expect(markerProblems([{ where: 'x', after: 'next' }], '2.8.2')[0]).toMatch(/not an X\.Y\.Z/);
  });

  it('takes the newer of the release and the module packages as released', () => {
    expect(compareVersions(releasedVersion, releaseManifest.release.semver)).toBeGreaterThanOrEqual(
      0,
    );
    expect(compareVersions(releasedVersion, MODULE_PACKAGE_VERSION)).toBeGreaterThanOrEqual(0);
  });
});

describe('next-release.json', () => {
  it('marks entries of the regions that can carry a marker', () => {
    const regions = { 'thin-module-directives': { a: '2.8.2' } };
    expect(nextReleaseMarks('thin-module-directives', ['a', 'b'], regions)).toEqual({ a: '2.8.2' });
  });

  it('refuses a name the reference data does not have', () => {
    const regions = { 'worker-flags': { gone: '2.8.2' } };
    expect(() => nextReleaseMarks('worker-flags', ['a'], regions)).toThrow(/unknown worker-flags/);
  });

  it('refuses module directives: the directive finder needs that region flat', () => {
    const regions = { 'module-directives': { ModPagespeedX: '2.8.2' } };
    expect(() => nextReleaseMarks('module-directives', ['ModPagespeedX'], regions)).toThrow(
      /module-directives entries \(ModPagespeedX\), but that region cannot be marked/,
    );
  });
});

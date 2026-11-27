// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Static guard for the page's privacy claim. The meter promises that nothing
// from a log leaves the browser and that the page's own code keeps nothing in
// storage. This test reads the source of the library, the page scripts and the
// page itself and fails if any of them names a network, storage or markup
// injection API. The browser tests check the behaviour; this check makes the
// claim hold in review before a line of it runs.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

const root = resolve(__dirname, '../..');
function walk(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const p = join(dir, name);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
}
const files = [
  ...walk(join(root, 'src/lib/crawler-cost')),
  ...walk(join(root, 'src/scripts/crawler-cost')),
  join(root, 'src/pages/ai-crawler-cost/index.astro'),
];
const forbidden = [
  'fetch(',
  'XMLHttpRequest',
  'sendBeacon',
  'WebSocket',
  'importScripts',
  'import(',
  'localStorage',
  'sessionStorage',
  'indexedDB',
  'caches.',
  'document.cookie',
  'innerHTML',
  'insertAdjacentHTML',
  'outerHTML',
  'createObjectURL',
];

describe('meter sources name no network, storage or markup-injection API', () => {
  it('finds the files to check', () => {
    expect(files.length).toBeGreaterThan(8);
  });
  for (const file of files) {
    it(file.slice(root.length + 1), () => {
      const text = readFileSync(file, 'utf8');
      const hits = forbidden.filter((token) => text.includes(token));
      expect(hits).toEqual([]);
    });
  }
});

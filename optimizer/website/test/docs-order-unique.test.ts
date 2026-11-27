// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// docs-order-unique.test.ts: within one sidebar group, every docs page needs its
// own `order`. Equal values leave the sidebar order to the sort's tie-break, which
// is how release notes once listed 2.4, 2.3, 2.5, 2.2, 2.6. Also pins the two
// orderings readers notice: release notes newest first, and the "Upgrade and
// migrate" hub ahead of the pages under it.

import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const dir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../src/content/docs');

type Doc = { file: string; group: string; order: number };

function readDocs(): Doc[] {
  return readdirSync(dir)
    .filter((f) => /\.mdx?$/.test(f))
    .map((file) => {
      const fm =
        /^---\r?\n([\s\S]*?)\r?\n---/.exec(readFileSync(path.join(dir, file), 'utf8'))?.[1] ?? '';
      const field = (k: string) =>
        new RegExp(`^${k}:\\s*['"]?([^'"\\r\\n]*?)['"]?\\s*$`, 'm').exec(fm)?.[1];
      return { file, group: field('group') ?? '', order: Number(field('order') ?? 0) };
    });
}

describe('docs sidebar order', () => {
  const docs = readDocs();

  it('order is unique within each sidebar group', () => {
    const seen = new Map<string, string>();
    const dupes: string[] = [];
    for (const d of docs) {
      if (!d.group) continue;
      const key = `${d.group}#${d.order}`;
      const prev = seen.get(key);
      if (prev) dupes.push(`${d.group}: order ${d.order} is used by ${prev} and ${d.file}`);
      else seen.set(key, d.file);
    }
    expect(dupes).toEqual([]);
  });

  it('release notes by line run newest first', () => {
    const notes = docs
      .filter((d) => d.group === 'Release notes by line')
      .map((d) => ({
        ver: /release-notes-(\d+)-(\d+)\./.exec(d.file)!.slice(1).map(Number),
        order: d.order,
      }))
      .sort((a, b) => a.order - b.order)
      .map((n) => n.ver.join('.'));
    const expected = [...notes].sort((a, b) => {
      const [am, an] = a.split('.').map(Number);
      const [bm, bn] = b.split('.').map(Number);
      return bm - am || bn - an;
    });
    expect(notes).toEqual(expected);
  });

  it('the "Upgrade and migrate" hub comes first in its group', () => {
    const group = docs
      .filter((d) => d.group === 'Upgrade and migrate')
      .sort((a, b) => a.order - b.order);
    expect(group[0].file).toBe('upgrade.mdx');
  });
});

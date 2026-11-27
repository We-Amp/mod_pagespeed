// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// recipes-files.test.ts: every /recipes/ entry that src/data/agent-files.mjs
// lists in llms.txt must be a file under public/, and every recipe file under
// public/recipes/ must be listed, so the index never points at a missing file
// and no recipe ships unlisted.

import { existsSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';

import { SURFACES } from '../../src/data/agent-files.mjs';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const PUBLIC = join(WEBSITE_ROOT, 'public');

const recipePaths: string[] = SURFACES.flatMap((s: { items: { path: string }[] }) =>
  s.items.map((i) => i.path),
).filter((p: string) => p.startsWith('/recipes/'));

describe('agent-files /recipes/ entries', () => {
  it('lists the recipes', () => {
    expect(recipePaths.length).toBeGreaterThanOrEqual(7);
  });

  it.each(recipePaths)('%s exists under public/', (p) => {
    const file = join(PUBLIC, p);
    expect(existsSync(file) && statSync(file).isFile()).toBe(true);
  });

  it('lists every recipe file under public/recipes/', () => {
    const onDisk = readdirSync(join(PUBLIC, 'recipes'))
      .filter((f) => f.endsWith('.md'))
      .map((f) => `/recipes/${f}`)
      .sort();
    expect([...recipePaths].sort()).toEqual(onDisk);
  });
});

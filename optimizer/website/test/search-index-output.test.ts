// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// search-index-output.test.ts — the check that stops a build whose Pagefind
// output is incomplete. Runs against a synthetic output directory, so it needs
// no build; the last test also checks the real output when one exists.

import { existsSync, mkdirSync, mkdtempSync, rmSync, unlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { findBadSearchOutput, REQUIRED_FILES } from '../scripts/lib/search-index-output.mjs';
import { PAGEFIND_BUNDLE_PATH } from '../src/lib/pagefind-bundle.mjs';

let dir: string;

function writeCompleteOutput(root: string) {
  mkdirSync(path.join(root, 'index'), { recursive: true });
  mkdirSync(path.join(root, 'fragment'), { recursive: true });
  for (const name of REQUIRED_FILES) writeFileSync(path.join(root, name), 'x');
  writeFileSync(path.join(root, 'wasm.en.pagefind'), 'x');
  writeFileSync(path.join(root, 'pagefind.en_0123456789.pf_meta'), 'x');
  writeFileSync(path.join(root, 'index', 'en_0123456.pf_index'), 'x');
  writeFileSync(path.join(root, 'fragment', 'en_0123456.pf_fragment'), 'x');
}

beforeEach(() => {
  dir = mkdtempSync(path.join(tmpdir(), 'search-index-output-'));
  writeCompleteOutput(dir);
});
afterEach(() => {
  rmSync(dir, { recursive: true, force: true });
});

describe('search index output check', () => {
  it('accepts a complete output', () => {
    expect(findBadSearchOutput(dir)).toEqual([]);
  });

  it('reports a missing output directory', () => {
    expect(findBadSearchOutput(path.join(dir, 'nope'))).toHaveLength(1);
  });

  for (const name of REQUIRED_FILES) {
    it(`reports ${name} when it is empty`, () => {
      writeFileSync(path.join(dir, name), '');
      expect(findBadSearchOutput(dir)).toEqual([`${name}: 0 bytes`]);
    });

    it(`reports ${name} when it is missing`, () => {
      unlinkSync(path.join(dir, name));
      expect(findBadSearchOutput(dir)).toEqual([`${name}: missing`]);
    });
  }

  it('reports an empty index or fragment directory', () => {
    unlinkSync(path.join(dir, 'index', 'en_0123456.pf_index'));
    unlinkSync(path.join(dir, 'fragment', 'en_0123456.pf_fragment'));
    expect(findBadSearchOutput(dir)).toEqual([
      'index/*.pf_index: none written',
      'fragment/*.pf_fragment: none written',
    ]);
  });

  it('reports a missing index directory, wasm file or metadata file', () => {
    rmSync(path.join(dir, 'index'), { recursive: true });
    unlinkSync(path.join(dir, 'wasm.en.pagefind'));
    unlinkSync(path.join(dir, 'pagefind.en_0123456789.pf_meta'));
    expect(findBadSearchOutput(dir)).toEqual([
      'wasm.*.pagefind: none written',
      '*.pf_meta: none written',
      'index/*.pf_index: none written',
    ]);
  });

  it('reports any 0-byte file, also one it has no name for', () => {
    writeFileSync(path.join(dir, 'fragment', 'en_empty.pf_fragment'), '');
    writeFileSync(path.join(dir, 'pagefind-highlight.js'), '');
    expect(findBadSearchOutput(dir).sort()).toEqual([
      'fragment/en_empty.pf_fragment: 0 bytes',
      'pagefind-highlight.js: 0 bytes',
    ]);
  });

  // After a build: the real output passes. Skipped without one.
  const built = fileURLToPath(new URL(`../dist/client${PAGEFIND_BUNDLE_PATH}`, import.meta.url));
  it.runIf(existsSync(built))('the built output is complete', () => {
    expect(findBadSearchOutput(built)).toEqual([]);
  });
});

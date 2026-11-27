// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Drift gate for the installed-base census post: every number, table and chart in
// the post's generated blocks must equal a fresh render from the census dataset.
// Fix a failure with: node scripts/generate-census-blocks.mjs --date <DATE>
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DATE = '2026-09-01';

describe('installed-base census post', () => {
  it('generated blocks match the census dataset', () => {
    const script = fileURLToPath(new URL('../scripts/generate-census-blocks.mjs', import.meta.url));
    expect(() =>
      execFileSync('node', [script, '--date', DATE, '--check'], {
        cwd: WEBSITE_ROOT,
        stdio: 'pipe',
      }),
    ).not.toThrow();
  });
});

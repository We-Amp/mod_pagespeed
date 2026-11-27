// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { gitLastModified } from './git-date';

describe('gitLastModified', () => {
  it('returns an ISO date for a tracked file, or null when history is unavailable', () => {
    // A full clone answers with the last commit date; a shallow CI checkout
    // or an exported tree must answer null rather than a misleading date.
    const date = gitLastModified('package.json');
    expect(date === null || /^\d{4}-\d{2}-\d{2}$/.test(date)).toBe(true);
  });

  it('returns null for a file that does not exist', () => {
    expect(gitLastModified('src/content/docs/does-not-exist.md')).toBeNull();
  });
});

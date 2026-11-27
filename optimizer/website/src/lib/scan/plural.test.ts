// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { plural } from './plural';

describe('plural', () => {
  it('counts the singular only for exactly one', () => {
    expect(plural(0, 'host')).toBe('0 hosts');
    expect(plural(1, 'host')).toBe('1 host');
    expect(plural(2, 'host')).toBe('2 hosts');
  });
  it('takes an irregular plural', () => {
    expect(plural(1, 'version tell')).toBe('1 version tell');
    expect(plural(3, 'entry', 'entries')).toBe('3 entries');
  });
});

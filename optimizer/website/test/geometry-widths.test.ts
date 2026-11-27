// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { ALL_WIDTHS, parseWidths } from '../tests/helpers/widths';

describe('geometry width selection', () => {
  it('unset, empty and "all" mean the full sweep', () => {
    expect(parseWidths(undefined)).toEqual(ALL_WIDTHS);
    expect(parseWidths('')).toEqual(ALL_WIDTHS);
    expect(parseWidths('all')).toEqual(ALL_WIDTHS);
  });
  it('a comma list narrows the sweep', () => {
    expect(parseWidths('375')).toEqual([375]);
    expect(parseWidths('375, 1280')).toEqual([375, 1280]);
  });
  it('rejects garbage', () => {
    expect(() => parseWidths('wide')).toThrow();
  });
});

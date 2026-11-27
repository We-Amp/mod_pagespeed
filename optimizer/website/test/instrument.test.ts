// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Unit tests for the readout helpers (src/lib/instrument.ts).

import { describe, it, expect } from 'vitest';

import {
  formatBytes,
  pctDelta,
  formatPct,
  gaugeWidth,
  monthYear,
  roundDownDisplay,
} from '../src/lib/instrument';

describe('formatBytes', () => {
  it('groups thousands en-US and suffixes a NBSP + B', () => {
    expect(formatBytes(38608)).toBe('38,608\u00a0B');
  });
  it('handles small values without a separator', () => {
    expect(formatBytes(999)).toBe('999\u00a0B');
  });
  it('handles zero', () => {
    expect(formatBytes(0)).toBe('0\u00a0B');
  });
});

describe('pctDelta', () => {
  it('returns a rounded integer percent, negative when smaller', () => {
    expect(pctDelta(99670, 23673)).toBe(-76);
    expect(pctDelta(56, 90)).toBe(61);
  });
  it('rounds halves toward +Infinity (Math.round semantics)', () => {
    expect(pctDelta(200, 101)).toBe(-49); // -49.5 -> -49
    expect(pctDelta(3, 2)).toBe(-33);
  });
  it('is 0 when nothing changed', () => {
    expect(pctDelta(10, 10)).toBe(0);
  });
  it('is 0 for a zero baseline instead of dividing by zero', () => {
    expect(pctDelta(0, 10)).toBe(0);
  });
  it('handles negative inputs as plain numbers', () => {
    expect(pctDelta(-100, -50)).toBe(-50);
  });
});

describe('formatPct', () => {
  it('uses U+2212, not a hyphen-minus, for negatives', () => {
    expect(formatPct(-29)).toBe('\u221229%');
    expect(formatPct(-29)).not.toContain('-');
  });
  it('renders zero without a sign', () => {
    expect(formatPct(0)).toBe('0%');
  });
  it('renders positives without a plus sign', () => {
    expect(formatPct(61)).toBe('61%');
  });
});

describe('gaugeWidth', () => {
  it('maps value into 0..100 of the range', () => {
    expect(gaugeWidth(56, 0, 100)).toBe(56);
    expect(gaugeWidth(0.8, 0, 2)).toBe(40);
  });
  it('clamps below-range and above-range values', () => {
    expect(gaugeWidth(-5, 0, 100)).toBe(0);
    expect(gaugeWidth(150, 0, 100)).toBe(100);
  });
  it('keeps 1 decimal', () => {
    expect(gaugeWidth(1, 0, 3)).toBe(33.3);
  });
  it('handles an inverted or empty range without NaN', () => {
    expect(gaugeWidth(5, 10, 10)).toBe(0);
    expect(gaugeWidth(5, 10, 0)).toBe(0);
  });
  it('handles negative ranges', () => {
    expect(gaugeWidth(-50, -100, 0)).toBe(50);
  });
});

describe('monthYear', () => {
  it('formats an ISO timestamp as "Month YYYY" in UTC', () => {
    expect(monthYear('2026-05-29T15:16:23.048445+00:00')).toBe('May 2026');
  });
  it('does not let a timezone shift the month', () => {
    // 03:30 at +04:00 on May 1 is still Apr 30 in UTC — the label follows UTC.
    expect(monthYear('2026-05-01T03:30:00+04:00')).toBe('April 2026');
  });
});

describe('roundDownDisplay', () => {
  it('rounds down to the step and appends +', () => {
    expect(roundDownDisplay(231341, 10000)).toBe('230,000+');
  });
  it('keeps an exact multiple', () => {
    expect(roundDownDisplay(230000, 10000)).toBe('230,000+');
  });
  it('rounds values below one step down to 0', () => {
    expect(roundDownDisplay(9999, 10000)).toBe('0+');
  });
  it('handles zero', () => {
    expect(roundDownDisplay(0, 10000)).toBe('0+');
  });
});

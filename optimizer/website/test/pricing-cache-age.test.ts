// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// pricing-cache-age.test.ts — the staleness check of scripts/fetch-pricing.js
// measures the cached data's age from its own fetchedAt timestamp, and its
// messages say so.

import { describe, it, expect } from 'vitest';
import {
  describePricingCacheAge,
  isPricingCacheFresh,
  pricingCacheAge,
} from '../scripts/lib/pricing-cache-age.mjs';

const NOW = Date.parse('2026-10-09T12:00:00.000Z');
const HOUR = 3600000;

describe('pricingCacheAge', () => {
  it('measures from fetchedAt, not from when the file was written', () => {
    const age = pricingCacheAge({ fetchedAt: '2026-06-13T20:26:56.809Z' }, NOW);
    expect(age?.fetchedAt).toBe('2026-06-13T20:26:56.809Z');
    expect(age!.hours).toBeCloseTo((NOW - Date.parse('2026-06-13T20:26:56.809Z')) / HOUR, 6);
    expect(age!.hours).toBeGreaterThan(2800);
  });

  it('is fresh just under the limit and stale at it', () => {
    const at = (h: number) => ({ fetchedAt: new Date(NOW - h * HOUR).toISOString() });
    expect(isPricingCacheFresh(pricingCacheAge(at(47.9), NOW), 48)).toBe(true);
    expect(isPricingCacheFresh(pricingCacheAge(at(48), NOW), 48)).toBe(false);
    expect(isPricingCacheFresh(pricingCacheAge(at(0), NOW), 48)).toBe(true);
  });

  it('counts a timestamp in the future as 0 hours old', () => {
    const age = pricingCacheAge({ fetchedAt: new Date(NOW + 5 * HOUR).toISOString() }, NOW);
    expect(age?.hours).toBe(0);
  });

  it('treats a missing or unparseable fetchedAt as unknown, and so stale', () => {
    for (const data of [{}, { fetchedAt: 'not a date' }, { fetchedAt: 42 }, null]) {
      const age = pricingCacheAge(data, NOW);
      expect(age).toBeNull();
      expect(isPricingCacheFresh(age, 48)).toBe(false);
    }
  });
});

describe('describePricingCacheAge', () => {
  it('names the timestamp the age was measured from', () => {
    const age = pricingCacheAge({ fetchedAt: new Date(NOW - 3 * HOUR).toISOString() }, NOW);
    expect(describePricingCacheAge(age, 48)).toBe(
      `fetched ${new Date(NOW - 3 * HOUR).toISOString()}, 3h ago by its fetchedAt timestamp; limit is 48h`,
    );
  });

  it('says when the age is unknown', () => {
    expect(describePricingCacheAge(null, 48)).toBe(
      'no valid fetchedAt timestamp, so its age is unknown; limit is 48h',
    );
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import type { StatsResponse } from '@pagespeed/api-client';
import { summarizeServeSavings } from '../savings';

type ServeSavings = NonNullable<StatsResponse['serve_savings']>;

/** Build a serve_savings block; omitted types default to all-zero. */
function makeServeSavings(p: Partial<ServeSavings> = {}): ServeSavings {
  const zero = { original_bytes: 0, optimized_bytes: 0, hits: 0 };
  return { html: zero, css: zero, js: zero, image: zero, ...p };
}

describe('summarizeServeSavings', () => {
  it('returns zeros (no NaN/negatives) when there is no data', () => {
    const s = summarizeServeSavings(undefined);
    expect(s.savedBytes).toBe(0);
    expect(s.savedPercent).toBe(0);
    expect(s.hits).toBe(0);
    expect(s.hasRegression).toBe(false);
  });

  it('reports real savings for the normal (optimized < original) case', () => {
    const s = summarizeServeSavings(
      makeServeSavings({
        css: { original_bytes: 1000, optimized_bytes: 250, hits: 4 },
        image: { original_bytes: 2000, optimized_bytes: 1000, hits: 6 },
      }),
    );
    expect(s.savedBytes).toBe(1750); // 3000 - 1250
    expect(s.savedPercent).toBe(58); // round(1750 / 3000 * 100)
    expect(s.hits).toBe(10);
    expect(s.hasRegression).toBe(false);
  });

  // The incident: critical-CSS inlining inflates served HTML past the pristine
  // origin, so the cross-type net is slightly negative. The dashboard hero must
  // NOT render a negative "bandwidth saved" headline — it clamps to 0, matching
  // the Savings page. (The honest signal is exposed via hasRegression.)
  it('clamps net to 0 when served bytes exceed origin (prod 2026-06-15 shape)', () => {
    const s = summarizeServeSavings(
      makeServeSavings({
        // Live we-amp.com worker /v1/stats, 2.0.25 @ 172df62.
        html: { original_bytes: 236_988_066, optimized_bytes: 254_841_427, hits: 1912 },
        css: { original_bytes: 13_601_434, optimized_bytes: 2_081_240, hits: 146 },
        image: { original_bytes: 1_825_425, optimized_bytes: 346_198, hits: 358 },
        js: { original_bytes: 2_479, optimized_bytes: 466, hits: 1 },
      }),
    );
    // sum(original) - sum(optimized) = -4,851,927  =>  must clamp, not render negative.
    expect(s.savedBytes).toBe(0);
    expect(s.savedPercent).toBe(0);
    expect(s.savedBytes).toBeGreaterThanOrEqual(0);
    expect(s.savedPercent).toBeGreaterThanOrEqual(0);
    // ...but the regression is surfaced honestly rather than hidden.
    expect(s.hasRegression).toBe(true);
    expect(s.hits).toBe(2417);
  });

  it('treats HTML-only inflation with no other wins as a clamped regression', () => {
    const s = summarizeServeSavings(
      makeServeSavings({
        html: { original_bytes: 22_177, optimized_bytes: 29_104, hits: 1 },
      }),
    );
    expect(s.savedBytes).toBe(0);
    expect(s.savedPercent).toBe(0);
    expect(s.hasRegression).toBe(true);
  });
});

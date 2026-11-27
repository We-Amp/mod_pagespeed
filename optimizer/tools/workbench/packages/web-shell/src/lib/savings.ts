// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { StatsResponse } from '@pagespeed/api-client';

export interface ServeSavingsSummary {
  /** Sum of origin bytes across all content types (uncompressed). */
  originalBytes: number;
  /** Sum of served (optimized) bytes across all content types (uncompressed). */
  optimizedBytes: number;
  /** Bandwidth saved, clamped to >= 0. Never negative in the headline. */
  savedBytes: number;
  /** Reduction percent, clamped to >= 0 and rounded. */
  savedPercent: number;
  /** Total serve hits across all content types. */
  hits: number;
  /**
   * True when served bytes exceed origin bytes (optimized > original), e.g.
   * when critical-CSS inlining trades payload bytes for render latency. The
   * headline clamps to 0, but callers can surface this honestly instead of
   * implying perfect savings.
   */
  hasRegression: boolean;
}

const EMPTY: ServeSavingsSummary = {
  originalBytes: 0,
  optimizedBytes: 0,
  savedBytes: 0,
  savedPercent: 0,
  hits: 0,
  hasRegression: false,
};

/**
 * Summarize serve-time bandwidth savings from the shared-mmap counters.
 *
 * Net savings is sum(original) - sum(optimized) across content types. Because
 * some optimizations (notably critical-CSS inlining) deliberately add bytes to
 * the served HTML, this delta can go slightly negative. The headline figures
 * (`savedBytes`, `savedPercent`) are clamped to >= 0 so the console never shows
 * a negative "bandwidth saved", matching the Savings page; `hasRegression`
 * preserves the honest signal.
 */
export function summarizeServeSavings(
  ss: StatsResponse['serve_savings'] | undefined,
): ServeSavingsSummary {
  if (!ss) return EMPTY;

  const entries = [ss.html, ss.css, ss.js, ss.image];
  let originalBytes = 0;
  let optimizedBytes = 0;
  let hits = 0;
  for (const e of entries) {
    if (!e) continue;
    originalBytes += e.original_bytes ?? 0;
    optimizedBytes += e.optimized_bytes ?? 0;
    hits += e.hits ?? 0;
  }

  const delta = originalBytes - optimizedBytes;
  const savedBytes = Math.max(0, delta);
  const savedPercent =
    originalBytes > 0 ? Math.max(0, Math.round((delta / originalBytes) * 100)) : 0;

  return {
    originalBytes,
    optimizedBytes,
    savedBytes,
    savedPercent,
    hits,
    hasRegression: optimizedBytes > originalBytes,
  };
}

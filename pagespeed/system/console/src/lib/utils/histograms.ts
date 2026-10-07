// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Presentation helpers for the Histograms page's bucket detail view.
 *
 * Kept out of the component so the behaviour is unit-testable.
 */

import { formatUnit } from "./format";

/** One bucket of a histogram, as served by GET /histograms. */
export interface Bucket {
  start: number;
  limit: number;
  count: number;
}

/**
 * Scale each bucket's count to a 0-100 bar width, relative to the busiest
 * bucket in the list. An empty list of buckets has no "largest" to scale
 * against, so it returns an empty list rather than dividing by zero.
 */
export function bucketWidths(buckets: Bucket[]): number[] {
  const max = Math.max(0, ...buckets.map((b) => b.count));
  return buckets.map((b) => (max ? Math.round((b.count / max) * 100) : 0));
}

/** A bucket's share of the histogram, and the running share so far. */
export interface BucketShare {
  percent: number;
  cumulative: number;
}

/**
 * Each bucket's percent of `total` (the histogram's overall count) and a
 * running cumulative percent through the list. `total` is passed in rather
 * than summed from `buckets` because the backend already skips zero-count
 * buckets -- summing only what's here would silently under-count whenever
 * that happened. A zero total has no percent to compute, so every share
 * comes back zero rather than NaN.
 */
export function bucketShares(buckets: Bucket[], total: number): BucketShare[] {
  let cumulative = 0;
  return buckets.map((b) => {
    const percent = total ? (b.count / total) * 100 : 0;
    cumulative += percent;
    return { percent, cumulative };
  });
}

/** The unit a histogram's values are expressed in. Histogram names are
 * display names ("Rewrite Latency ms"), so the unit word can trail the
 * name as well as ride a raw counter suffix; neither is an error. */
export function unitForHistogram(name: string): string {
  const suffixed = formatUnit(name);
  if (suffixed !== null) return suffixed;
  if (/\bus$/i.test(name)) return "µs";
  if (/\bms$/i.test(name)) return "ms";
  return "—";
}

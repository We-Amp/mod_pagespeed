// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Merge the statistics log's history with the live ring buffer's tail into
 * one series. Both sides arrive as parallel timestamp/value arrays in epoch
 * seconds (the log's milliseconds are converted at the fetch boundary). The
 * union of both timestamp sets is sorted ascending; when both sides carry
 * the same second the live sample wins — it is fresher than the log entry
 * the module happened to write for that second. A null on either side is a
 * known gap (a counter missing from one log segment) and stays a gap.
 */

import type { Sample } from "./graph-series";

export interface MergedSeries {
  /** Epoch seconds, ascending. */
  timestamps: number[];
  /** One value per timestamp; null is a gap. */
  values: Sample[];
  /** Epoch seconds of the oldest live sample, or null when log-only. */
  liveSince: number | null;
  /** True when the log contributed no points. */
  liveOnly: boolean;
}

export function mergeSeries(
  logTimestampsSec: readonly number[],
  logValues: readonly Sample[],
  liveTimestampsSec: readonly number[],
  liveValues: readonly Sample[],
): MergedSeries {
  const bySecond = new Map<number, Sample>();
  const logCount = Math.min(logTimestampsSec.length, logValues.length);
  for (let i = 0; i < logCount; i++) {
    bySecond.set(logTimestampsSec[i], logValues[i]);
  }
  let liveSince: number | null = null;
  const liveCount = Math.min(liveTimestampsSec.length, liveValues.length);
  for (let i = 0; i < liveCount; i++) {
    const t = liveTimestampsSec[i];
    bySecond.set(t, liveValues[i]); // a live sample wins a same-second tie
    if (liveSince === null || t < liveSince) liveSince = t;
  }
  const timestamps = [...bySecond.keys()].sort((a, b) => a - b);
  return {
    timestamps,
    values: timestamps.map((t) => bySecond.get(t) ?? null),
    liveSince,
    liveOnly: logCount === 0,
  };
}

/**
 * The suffix of the parallel arrays whose timestamp is at or after
 * `sinceSec`: the part of a series inside the selected range. The inputs
 * ascend, so the cut is one index; mismatched lengths are read to the
 * shorter one, and an empty window is two empty arrays.
 */
export function sliceSince(
  timestampsSec: readonly number[],
  values: readonly Sample[],
  sinceSec: number,
): { timestamps: number[]; values: Sample[] } {
  const n = Math.min(timestampsSec.length, values.length);
  let start = n;
  while (start > 0 && timestampsSec[start - 1] >= sinceSec) start--;
  return {
    timestamps: timestampsSec.slice(start, n),
    values: values.slice(start, n),
  };
}

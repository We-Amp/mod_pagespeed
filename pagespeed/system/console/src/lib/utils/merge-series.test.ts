// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { mergeSeries, sliceSince } from "./merge-series";

describe("mergeSeries", () => {
  it("returns the log alone when there is no live data yet", () => {
    const m = mergeSeries([60, 120], [1, 2], [], []);
    expect(m.timestamps).toEqual([60, 120]);
    expect(m.values).toEqual([1, 2]);
    expect(m.liveSince).toBeNull();
    expect(m.liveOnly).toBe(false);
  });

  it("appends the live tail after the log", () => {
    const m = mergeSeries([60, 120], [1, 2], [125, 130], [3, 4]);
    expect(m.timestamps).toEqual([60, 120, 125, 130]);
    expect(m.values).toEqual([1, 2, 3, 4]);
    expect(m.liveSince).toBe(125);
    expect(m.liveOnly).toBe(false);
  });

  it("lets a live sample win a same-second tie with the log", () => {
    const m = mergeSeries([60, 120], [1, 2], [120, 130], [20, 3]);
    expect(m.timestamps).toEqual([60, 120, 130]);
    expect(m.values).toEqual([1, 20, 3]);
  });

  it("keeps a log gap the live window does not cover", () => {
    // A counter missing from one statistics-log segment is null there.
    const m = mergeSeries([60, 120, 180], [1, null, 3], [185], [4]);
    expect(m.values).toEqual([1, null, 3, 4]);
  });

  it("sorts an interleaved live sample into place", () => {
    // A live sample can predate the newest log sample when the logging
    // interval has just rolled over; the union stays timestamp-sorted.
    const m = mergeSeries([100, 200], [1, 2], [150], [9]);
    expect(m.timestamps).toEqual([100, 150, 200]);
    expect(m.values).toEqual([1, 9, 2]);
    expect(m.liveSince).toBe(150);
  });

  it("marks a series the log does not have as live-only", () => {
    const m = mergeSeries([], [], [100, 105], [7, 8]);
    expect(m.timestamps).toEqual([100, 105]);
    expect(m.values).toEqual([7, 8]);
    expect(m.liveSince).toBe(100);
    expect(m.liveOnly).toBe(true);
  });

  it("reads mismatched lengths defensively", () => {
    const m = mergeSeries([60], [1, 2, 3], [], []);
    expect(m.timestamps).toEqual([60]);
    expect(m.values).toEqual([1]);
    expect(m.liveOnly).toBe(false);
  });
});

describe("sliceSince", () => {
  it("keeps everything when the range starts before the first sample", () => {
    expect(sliceSince([60, 120], [1, 2], 30)).toEqual({ timestamps: [60, 120], values: [1, 2] });
  });

  it("drops the samples older than the range", () => {
    expect(sliceSince([60, 120, 180], [1, 2, 3], 120)).toEqual({ timestamps: [120, 180], values: [2, 3] });
  });

  it("keeps a sample exactly at the range's start", () => {
    expect(sliceSince([60, 120], [1, 2], 60)).toEqual({ timestamps: [60, 120], values: [1, 2] });
  });

  it("returns empty arrays when every sample is older than the range", () => {
    expect(sliceSince([60, 120], [1, 2], 121)).toEqual({ timestamps: [], values: [] });
  });

  it("reads mismatched lengths to the shorter one", () => {
    expect(sliceSince([60, 120, 180], [1, 2], 120)).toEqual({ timestamps: [120], values: [2] });
  });
});

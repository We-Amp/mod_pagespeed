// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { hasHistory, historyCadenceText, widerRange } from "./graph-range";
import type { GraphsView } from "./graphs-view";

// A real statistics-log cadence: about 600 s between samples (gaps of 476-632 s).
const T0 = Date.UTC(2026, 9, 29, 16, 0, 0);
const GAPS_S = [600, 590, 632, 476, 600, 610];
const timestamps = GAPS_S.reduce<number[]>((acc, g) => [...acc, acc[acc.length - 1] + g * 1000], [T0]);
const data = (ts: number[]): GraphsView => ({ kind: "data", timestamps: ts, graphs: [] });

describe("hasHistory", () => {
  it("needs two logged points", () => {
    expect(hasHistory(data(timestamps))).toBe(true);
    expect(hasHistory(data([T0]))).toBe(false);
    expect(hasHistory({ kind: "empty-range" })).toBe(false);
    expect(hasHistory({ kind: "no-log" })).toBe(false);
    expect(hasHistory(null)).toBe(false);
  });
});

describe("widerRange", () => {
  it("steps through the selectable ranges and stops at the widest", () => {
    expect(widerRange(5)).toBe(15);
    expect(widerRange(15)).toBe(30);
    expect(widerRange(360)).toBe(1440);
    expect(widerRange(1440)).toBeNull();
  });
  it("a 15-minute range holds at most two of the ~10-minute samples, so a viewer who never chose gets a wider one", () => {
    const inFifteen = timestamps.filter((t) => t >= timestamps[timestamps.length - 1] - 15 * 60_000);
    expect(inFifteen.length).toBeLessThanOrEqual(2);
    expect(widerRange(15)).toBe(30);
  });
});

describe("historyCadenceText", () => {
  it("states the log cadence from the real gaps", () => {
    expect(historyCadenceText(data(timestamps))).toBe("History every 10 min");
  });
  it("says nothing without two points or without data", () => {
    expect(historyCadenceText(data([T0]))).toBeNull();
    expect(historyCadenceText({ kind: "empty-range" })).toBeNull();
    expect(historyCadenceText(null)).toBeNull();
  });
});

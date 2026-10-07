// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import { bucketWidths, bucketShares, unitForHistogram } from "./histograms";

describe("bucketWidths", () => {
  it("scales bar widths to the largest bucket", () => {
    expect(
      bucketWidths([
        { start: 0, limit: 10, count: 2 },
        { start: 10, limit: 20, count: 4 },
      ]),
    ).toEqual([50, 100]);
  });

  it("returns an empty list for no buckets", () => {
    expect(bucketWidths([])).toEqual([]);
  });
});

describe("bucketShares", () => {
  it("computes each bucket's percent of the total and a running cumulative percent", () => {
    expect(
      bucketShares(
        [
          { start: 0, limit: 10, count: 2 },
          { start: 10, limit: 20, count: 4 },
          { start: 20, limit: 30, count: 4 },
        ],
        10,
      ),
    ).toEqual([
      { percent: 20, cumulative: 20 },
      { percent: 40, cumulative: 60 },
      { percent: 40, cumulative: 100 },
    ]);
  });

  it("returns an empty list for no buckets", () => {
    expect(bucketShares([], 0)).toEqual([]);
  });

  it("returns zero shares when the total is zero", () => {
    expect(bucketShares([{ start: 0, limit: 10, count: 0 }], 0)).toEqual([
      { percent: 0, cumulative: 0 },
    ]);
  });
});

describe("unitForHistogram", () => {
  it("reads the unit from the display name's last word", () => {
    expect(unitForHistogram("Html Time us")).toBe("µs");
    expect(unitForHistogram("Rewrite Latency ms")).toBe("ms");
  });

  it("also reads it from a raw counter suffix", () => {
    expect(unitForHistogram("rewrite_latency_ms")).toBe("ms");
    expect(unitForHistogram("cache_size_kb")).toBe("KB");
    expect(unitForHistogram("image_inline_max_bytes")).toBe("B");
  });

  it("an honest dash when the name carries no unit", () => {
    expect(unitForHistogram("Requests")).toBe("—");
    expect(unitForHistogram("")).toBe("—");
  });
});

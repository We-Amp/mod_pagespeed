// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  alignedData,
  axisFont,
  bytesRateTicks,
  chartWindowLabel,
  seriesSummary,
  timeTickLabel,
  valueAxisWidth,
} from "./chart-summary";

describe("alignedData", () => {
  it("puts timestamps first, then one column per series", () => {
    expect(
      alignedData([1, 2], [
        { label: "a", data: [10, 20] },
        { label: "b", data: [null, 7] },
      ]),
    ).toEqual([
      [1, 2],
      [10, 20],
      [null, 7],
    ]);
  });
});

describe("seriesSummary", () => {
  it("says so when there is no data", () => {
    expect(seriesSummary("Traffic", [])).toBe("Traffic: no data");
  });

  it("reports the latest non-null value and the point count per series", () => {
    const text = seriesSummary("Savings rate (per second)", [
      { label: "Module rewrites", data: [3, 5, null] },
      { label: "Optimizer cache serves", data: [null, null] },
    ]);
    expect(text).toBe(
      "Savings rate (per second): Module rewrites latest 5, 2 points, Optimizer cache serves latest no data, 0 points",
    );
  });

  it("formats the latest value with the caller's formatter and says \"point\" once", () => {
    const text = seriesSummary(
      "Savings rate (per second)",
      [{ label: "Module rewrites", data: [5] }],
      (v) => `${v}/s`,
    );
    expect(text).toBe("Savings rate (per second): Module rewrites latest 5/s, 1 point");
  });

  it("ignores non-finite values when looking for the latest", () => {
    expect(seriesSummary("T", [{ label: "a", data: [4, Number.NaN as never] }])).toBe("T: a latest 4, 1 point");
  });
});

describe("axisFont", () => {
  // The chart library reads the label size from a "<n>px" token in the axis
  // font; without one it lays out no tick labels at all.
  it("prefixes a pixel size to a bare font family", () => {
    expect(axisFont("'JetBrains Mono', monospace")).toBe("12px 'JetBrains Mono', monospace");
  });

  it("keeps a font that already names its size", () => {
    expect(axisFont("11px monospace")).toBe("11px monospace");
  });

  it("falls back to monospace for an empty family", () => {
    expect(axisFont("")).toBe("12px monospace");
    expect(axisFont("   ")).toBe("12px monospace");
  });
});

describe("timeTickLabel", () => {
  // 2026-09-30 13:31:05 UTC; the tests pin the time zone through the formatter's zone argument.
  const t = Date.UTC(2026, 8, 30, 13, 31, 5) / 1000;

  it("shows hours and minutes when ticks are a minute or more apart", () => {
    expect(timeTickLabel(t, 60, "UTC")).toBe("13:31");
    expect(timeTickLabel(t, 1800, "UTC")).toBe("13:31");
  });

  it("adds seconds when ticks are less than a minute apart", () => {
    expect(timeTickLabel(t, 5, "UTC")).toBe("13:31:05");
  });

  it("is empty for a missing tick", () => {
    expect(timeTickLabel(null, 60, "UTC")).toBe("");
  });
});

describe("chartWindowLabel", () => {
  it("names the covered window and the sample interval", () => {
    const ts: number[] = [];
    for (let i = 0; i < 36; ++i) ts.push(i * 10);
    expect(chartWindowLabel(ts)).toBe("Last 6 min · 10 s samples");
  });

  it("uses seconds for short windows and minute samples for slow ones", () => {
    expect(chartWindowLabel([0, 10, 20])).toBe("Last 20 s · 10 s samples");
    const slow: number[] = [];
    for (let i = 0; i < 12; ++i) slow.push(i * 60);
    expect(chartWindowLabel(slow)).toBe("Last 11 min · 1 min samples");
  });

  it("says it is still collecting under two samples", () => {
    expect(chartWindowLabel([])).toBe("collecting samples");
    expect(chartWindowLabel([10])).toBe("collecting samples");
  });
});

describe("bytesRateTicks", () => {
  it("a flat series: repeated ticks need no decimals", () => {
    expect(bytesRateTicks([5, 5, 5])).toEqual(["5 B/s", "5 B/s", "5 B/s"]);
    expect(bytesRateTicks([0, 0, 0, null])).toEqual(["0 B/s", "0 B/s", "0 B/s", ""]);
    expect(bytesRateTicks([2048, 2048, 4096])).toEqual(["2 KB/s", "2 KB/s", "4 KB/s"]);
  });

  it("0–50 B/s: bytes, whole numbers", () => {
    expect(bytesRateTicks([0, 10, 20, 30, 40, 50])).toEqual(["0 B/s", "10 B/s", "20 B/s", "30 B/s", "40 B/s", "50 B/s"]);
  });

  it("0–5 KB/s: kilobytes, every tick distinct", () => {
    expect(bytesRateTicks([0, 1000, 2000, 3000, 4000, 5000])).toEqual([
      "0 KB/s",
      "1 KB/s",
      "2 KB/s",
      "3 KB/s",
      "4 KB/s",
      "5 KB/s",
    ]);
  });

  it("0–3 MB/s: megabytes, one decimal where whole numbers would repeat", () => {
    expect(bytesRateTicks([0, 500_000, 1_000_000, 1_500_000, 2_000_000, 2_500_000, 3_000_000])).toEqual([
      "0.0 MB/s",
      "0.5 MB/s",
      "1.0 MB/s",
      "1.4 MB/s",
      "1.9 MB/s",
      "2.4 MB/s",
      "2.9 MB/s",
    ]);
  });

  it("a tiny range never flattens to one label", () => {
    const ticks = bytesRateTicks([0, 0.1, 0.2, 0.3, 0.4]);
    expect(new Set(ticks).size).toBe(5);
    expect(ticks.every((t) => t.endsWith(" B/s"))).toBe(true);
    // 2.000–2.003 KB/s: whole numbers, one and two decimals all repeat; three separate them.
    expect(bytesRateTicks([2048, 2049, 2050, 2051])).toEqual(["2.000 KB/s", "2.001 KB/s", "2.002 KB/s", "2.003 KB/s"]);
  });

  it("a missing split is a blank label", () => {
    expect(bytesRateTicks([null, 10, 20])).toEqual(["", "10 B/s", "20 B/s"]);
  });
});

describe("valueAxisWidth", () => {
  // A stand-in for the canvas: 7 px per character.
  const measure = (s: string) => s.length * 7;

  it("reserves the widest label plus the tick and gap", () => {
    // "1.50 KB/s" is 9 characters = 63 px; with 15 px of tick and gap, 78.
    expect(valueAxisWidth(["0 B/s", "1.50 KB/s", "600 B/s"], measure, 15, 50)).toBe(78);
  });

  it("never goes below the minimum for short labels", () => {
    expect(valueAxisWidth(["0", "5", "10"], measure, 15, 50)).toBe(50);
  });

  it("ignores empty and missing labels", () => {
    expect(valueAxisWidth(["", null, undefined, "12"], measure, 15, 50)).toBe(50);
    expect(valueAxisWidth([], measure, 15, 50)).toBe(50);
    expect(valueAxisWidth(null, measure, 15, 50)).toBe(50);
  });

  it("rounds a fractional width up so the number is never clipped", () => {
    expect(valueAxisWidth(["123456789"], () => 60.2, 15, 50)).toBe(76);
  });

  it("falls back to the minimum when a measurement is not a finite number", () => {
    expect(valueAxisWidth(["1.50 KB/s"], () => Number.NaN, 15, 50)).toBe(50);
    expect(valueAxisWidth(["1.50 KB/s"], () => Number.POSITIVE_INFINITY, 15, 50)).toBe(50);
    expect(valueAxisWidth(["1.50 KB/s"], measure, Number.NaN, 50)).toBe(50);
  });
});

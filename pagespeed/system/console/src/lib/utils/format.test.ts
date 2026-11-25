// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  formatBytes,
  formatBytesRate,
  formatClock,
  formatCount,
  formatDuration,
  formatHistorySpan,
  formatIsoTitle,
  formatPercent,
  formatRelative,
  formatSpan,
  formatSig,
  formatSignedBytesRate,
  formatUnit,
} from "./format";

describe("formatBytes", () => {
  it("formats at 3 significant digits", () => {
    expect(formatBytes(0)).toBe("0 B");
    expect(formatBytes(512)).toBe("512 B");
    expect(formatBytes(1024)).toBe("1.00 KB");
    expect(formatBytes(1536)).toBe("1.50 KB");
    expect(formatBytes(110554)).toBe("108 KB");
    expect(formatBytes(221184)).toBe("216 KB");
    expect(formatBytes(2306867)).toBe("2.20 MB");
    expect(formatBytes(1.5 * 1024 ** 3)).toBe("1.50 GB");
    expect(formatBytes(2 * 1024 ** 4)).toBe("2.00 TB");
  });
  it("renders missing and nonsense input as an em dash", () => {
    expect(formatBytes(undefined)).toBe("—");
    expect(formatBytes(null)).toBe("—");
    expect(formatBytes(NaN)).toBe("—");
    expect(formatBytes(-5)).toBe("—");
  });
});

describe("formatPercent", () => {
  it("renders no data as an em dash and clamps to [0, 100]", () => {
    expect(formatPercent(undefined)).toBe("—");
    expect(formatPercent(null)).toBe("—");
    expect(formatPercent(NaN)).toBe("—");
    expect(formatPercent(130)).toBe("100%");
    expect(formatPercent(-3)).toBe("0%");
  });
  it("never rounds a real saving down to 0%", () => {
    expect(formatPercent(0)).toBe("0%");
    expect(formatPercent(0.4)).toBe("<1%");
    expect(formatPercent(0.5)).toBe("1%");
    expect(formatPercent(45.6)).toBe("46%");
    expect(formatPercent(100)).toBe("100%");
  });
  it("shows a rounded-away real saving as under one percent, never zero", () => {
    // The second argument is the quantity behind an already-rounded
    // percentage: a nonzero amount that rounded to 0 still reads "<1%".
    expect(formatPercent(0, 4321)).toBe("<1%");
    expect(formatPercent(0, 1)).toBe("<1%");
    expect(formatPercent(0, 0)).toBe("0%");
    expect(formatPercent(12, 512)).toBe("12%");
    expect(formatPercent(null, 512)).toBe("—");
  });
});

describe("formatCount", () => {
  it("groups thousands, pinned to en-US", () => {
    expect(formatCount(0)).toBe("0");
    expect(formatCount(1234567)).toBe("1,234,567");
  });
  it("renders missing input as an em dash", () => {
    expect(formatCount(undefined)).toBe("—");
    expect(formatCount(null)).toBe("—");
    expect(formatCount(NaN)).toBe("—");
  });
});

describe("formatRelative", () => {
  const now = 1_800_000_000_000;
  it("scales seconds to days", () => {
    expect(formatRelative(now, now)).toBe("0 s ago");
    expect(formatRelative(now - 12_000, now)).toBe("12 s ago");
    expect(formatRelative(now - 60_000, now)).toBe("1 min ago");
    expect(formatRelative(now - 5 * 60_000, now)).toBe("5 min ago");
    expect(formatRelative(now - 3_600_000, now)).toBe("1 h ago");
    expect(formatRelative(now - 3 * 3_600_000, now)).toBe("3 h ago");
    expect(formatRelative(now - 86_400_000, now)).toBe("1 d ago");
    expect(formatRelative(now - 2 * 86_400_000, now)).toBe("2 d ago");
  });
  it("clamps a future timestamp to 0 s ago", () => {
    expect(formatRelative(now + 5_000, now)).toBe("0 s ago");
  });
});

describe("formatHistorySpan", () => {
  const MIN = 60;
  const HOUR = 3_600;
  const DAY = 86_400;
  it("under an hour: the nearest minute, About when rounded", () => {
    expect(formatHistorySpan(0)).toBe("Under 1 min");
    expect(formatHistorySpan(29)).toBe("Under 1 min");
    expect(formatHistorySpan(42)).toBe("About 1 min");
    expect(formatHistorySpan(59 * MIN)).toBe("59 min");
    expect(formatHistorySpan(59 * MIN + 10)).toBe("About 59 min");
  });
  it("rounding up to the hour is stated in hours", () => {
    expect(formatHistorySpan(59 * MIN + 40)).toBe("About 1 h");
    expect(formatHistorySpan(HOUR)).toBe("1 h");
    expect(formatHistorySpan(HOUR + 3)).toBe("About 1 h");
  });
  it("under a day: hours and the nearest 10 minutes, never rounded down to the hour", () => {
    expect(formatHistorySpan(HOUR + 39 * MIN)).toBe("About 1 h 40 min");
    expect(formatHistorySpan(HOUR + 40 * MIN)).toBe("1 h 40 min");
    expect(formatHistorySpan(9 * HOUR)).toBe("9 h");
    expect(formatHistorySpan(5 * HOUR + 3 * MIN)).toBe("About 5 h");
    expect(formatHistorySpan(23 * HOUR + 50 * MIN)).toBe("23 h 50 min");
  });
  it("rounding up to the day is stated in days", () => {
    expect(formatHistorySpan(23 * HOUR + 59 * MIN)).toBe("About 1 d");
    expect(formatHistorySpan(DAY)).toBe("1 d");
  });
  it("from a day on: days and the nearest hour", () => {
    expect(formatHistorySpan(DAY + 5)).toBe("About 1 d");
    expect(formatHistorySpan(DAY + 3 * HOUR)).toBe("1 d 3 h");
    expect(formatHistorySpan(DAY + 3 * HOUR + 31 * MIN)).toBe("About 1 d 4 h");
    expect(formatHistorySpan(2 * DAY + 23 * HOUR + 40 * MIN)).toBe("About 3 d");
    expect(formatHistorySpan(30 * DAY)).toBe("30 d");
  });
  it("a negative or non-finite span is no span", () => {
    expect(formatHistorySpan(-4)).toBe("Under 1 min");
    expect(formatHistorySpan(Number.NaN)).toBe("Under 1 min");
  });
});

describe("formatSpan", () => {
  it("says a span in the console's sentence words, rounded down", () => {
    expect(formatSpan(42)).toBe("42 s");
    expect(formatSpan(125)).toBe("2 min");
    expect(formatSpan(7300)).toBe("2 h");
    expect(formatSpan(3 * 86_400 + 5)).toBe("3 d");
    expect(formatSpan(-4)).toBe("0 s");
  });
});

describe("formatIsoTitle", () => {
  it("is the ISO string", () => {
    expect(formatIsoTitle(0)).toBe("1970-01-01T00:00:00.000Z");
  });
});

describe("formatDuration", () => {
  it("composes d/h/m/s", () => {
    expect(formatDuration(0)).toBe("0s");
    expect(formatDuration(12)).toBe("12s");
    expect(formatDuration(41 * 60 + 9)).toBe("41m 9s");
    expect(formatDuration(5 * 3600 + 3 * 60)).toBe("5h 3m");
    expect(formatDuration(2 * 86400 + 5 * 3600 + 3 * 60)).toBe("2d 5h 3m");
  });
  it("renders missing and nonsense input as an em dash", () => {
    expect(formatDuration(undefined)).toBe("—");
    expect(formatDuration(NaN)).toBe("—");
    expect(formatDuration(-1)).toBe("—");
  });
});

describe("formatUnit", () => {
  it("reads the unit from the counter suffix", () => {
    expect(formatUnit("rewrite_latency_ms")).toBe("ms");
    expect(formatUnit("cache_latency_us")).toBe("µs");
    expect(formatUnit("image_inline_max_bytes")).toBe("B");
    expect(formatUnit("cache_size_kb")).toBe("KB");
    expect(formatUnit("requests")).toBeNull();
  });
});

describe("formatSig", () => {
  it("keeps the requested significant digits", () => {
    expect(formatSig(58.1313)).toBe("58.1");
    expect(formatSig(1.5)).toBe("1.50");
    expect(formatSig(0.0456)).toBe("0.0456");
    expect(formatSig(0)).toBe("0");
    expect(formatSig(-12.345)).toBe("-12.3");
  });
  it("groups integers that outgrow the digit budget", () => {
    expect(formatSig(6870.29)).toBe("6,870");
    expect(formatSig(1234567)).toBe("1,230,000");
  });
  it("honours an explicit digit budget", () => {
    expect(formatSig(1234.5, 2)).toBe("1,200");
    expect(formatSig(0.00456, 2)).toBe("0.0046");
  });
  it("renders non-finite input as an em dash", () => {
    expect(formatSig(NaN)).toBe("—");
    expect(formatSig(Infinity)).toBe("—");
  });
});

describe("formatClock", () => {
  it("is the local wall-clock time, HH:MM:SS", () => {
    expect(formatClock(new Date(2026, 9, 2, 10, 25, 51).getTime())).toBe("10:25:51");
    expect(formatClock(new Date(2026, 0, 1, 1, 2, 3).getTime())).toBe("01:02:03");
  });
  it("is a dash when there is no time to show", () => {
    expect(formatClock(0)).toBe("—");
    expect(formatClock(-5)).toBe("—");
    expect(formatClock(Number.NaN)).toBe("—");
    expect(formatClock(1e16)).toBe("—");
  });
});

describe("formatBytesRate", () => {
  it("is the byte formatter with /s", () => {
    expect(formatBytesRate(10055)).toBe("9.82 KB/s");
    // Signed: a negative rate keeps its figure.
    expect(formatSignedBytesRate(10055)).toBe("9.82 KB/s");
    expect(formatSignedBytesRate(-10055)).toBe("−9.82 KB/s");
    expect(formatSignedBytesRate(0)).toBe("0 B/s");
    expect(formatSignedBytesRate(null)).toBe("—");
    expect(formatSignedBytesRate(Number.NaN)).toBe("—");
    expect(formatBytesRate(0)).toBe("0 B/s");
    expect(formatBytesRate(512)).toBe("512 B/s");
    expect(formatBytesRate(3 * 1024 * 1024)).toBe("3.00 MB/s");
  });

  it("no value is a dash, never a unit on nothing", () => {
    for (const v of [null, undefined, Number.NaN, -1, Number.POSITIVE_INFINITY]) {
      expect(formatBytesRate(v)).toBe("—");
    }
  });
});

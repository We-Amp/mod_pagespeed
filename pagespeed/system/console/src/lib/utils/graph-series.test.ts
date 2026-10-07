// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import {
  DELTA_VIEW_STORAGE_KEY,
  formatRate,
  graphTitle,
  latestForDisplay,
  loadDeltaView,
  saveDeltaView,
  seriesForDisplay,
  toRatesPerSecond,
} from "./graph-series";

/** Minimal in-memory Storage stand-in. */
function fakeStorage(initial: Record<string, string> = {}): Storage {
  const map = new Map(Object.entries(initial));
  return {
    get length() {
      return map.size;
    },
    clear: () => map.clear(),
    getItem: (k: string) => (map.has(k) ? map.get(k)! : null),
    key: (i: number) => [...map.keys()][i] ?? null,
    removeItem: (k: string) => void map.delete(k),
    setItem: (k: string, v: string) => void map.set(k, String(v)),
  } as Storage;
}

/** Storage that throws on every access, like Safari with cookies blocked. */
function throwingStorage(): Storage {
  const boom = () => {
    throw new Error("SecurityError: storage is not available");
  };
  return {
    get length(): number {
      return boom();
    },
    clear: boom,
    getItem: boom,
    key: boom,
    removeItem: boom,
    setItem: boom,
  } as unknown as Storage;
}

describe("seriesForDisplay", () => {
  it("plots the cumulative values unchanged when the view is off", () => {
    expect(seriesForDisplay([0, 5, 10], [10, 13, 5], false)).toEqual([10, 13, 5]);
  });

  it("plots rates per second when the view is on", () => {
    expect(seriesForDisplay([0, 5, 10], [10, 40, 70], true)).toEqual([null, 6, 6]);
  });

  it("does not mutate the input series", () => {
    const values = [1, 2, 3];
    seriesForDisplay([0, 1, 2], values, true);
    seriesForDisplay([0, 1, 2], values, false);
    expect(values).toEqual([1, 2, 3]);
  });
});

describe("latestForDisplay", () => {
  it("reports the newest value when the final sample has one", () => {
    expect(latestForDisplay([null, 3, 0, 7])).toBe(7);
  });

  it("reports the newest value even when the final sample is a gap", () => {
    // The range ends on a counter reset: the rate right after it is a gap
    // the chart draws, and the rate up to the restart is still the latest
    // reading.
    expect(latestForDisplay([null, 40, null])).toBe(40);
  });

  it("reports nothing for a series that is only gaps", () => {
    expect(latestForDisplay([null, null])).toBeNull();
  });

  it("reports nothing for an empty series", () => {
    expect(latestForDisplay([])).toBeNull();
  });

  it("reports a final zero rather than treating it as missing", () => {
    expect(latestForDisplay([null, 5, 0])).toBe(0);
  });

  it("reports the last cumulative value when the view is off", () => {
    expect(latestForDisplay(seriesForDisplay([0, 5, 10], [10, 13, 20], false))).toBe(20);
  });

  it("reports the rate up to the restart when a reset ends the range in the rate view", () => {
    expect(latestForDisplay(seriesForDisplay([0, 5, 10], [100, 140, 5], true))).toBe(8);
  });
});

describe("graphTitle", () => {
  it("marks the title when the rate view is on", () => {
    expect(graphTitle("http.requests", true)).toBe("http.requests (per second)");
  });

  it("leaves the title alone when the view is off", () => {
    expect(graphTitle("http.requests", false)).toBe("http.requests");
  });
});

describe("delta-view persistence", () => {
  it("is unset when nothing was ever stored", () => {
    expect(loadDeltaView(fakeStorage())).toBeNull();
  });

  it("round-trips the toggle through storage", () => {
    const storage = fakeStorage();
    saveDeltaView(true, storage);
    expect(storage.getItem(DELTA_VIEW_STORAGE_KEY)).toBe("1");
    expect(loadDeltaView(storage)).toBe(true);

    saveDeltaView(false, storage);
    expect(loadDeltaView(storage)).toBe(false);
  });

  it("is unset for an unrecognised stored value", () => {
    expect(loadDeltaView(fakeStorage({ [DELTA_VIEW_STORAGE_KEY]: "yes" }))).toBeNull();
    expect(loadDeltaView(fakeStorage({ [DELTA_VIEW_STORAGE_KEY]: "" }))).toBeNull();
  });

  it("is unset when storage is absent", () => {
    expect(loadDeltaView(null)).toBeNull();
  });

  it("is unset outside a browser, where there is no localStorage", () => {
    // No argument, so the default-storage path runs. Under the node test
    // environment there is no `localStorage`.
    expect(loadDeltaView()).toBeNull();
  });

  it("is unset, and does not throw, when storage access throws", () => {
    expect(loadDeltaView(throwingStorage())).toBeNull();
  });

  it("does not throw when persisting into unavailable storage", () => {
    expect(() => saveDeltaView(true, throwingStorage())).not.toThrow();
    expect(() => saveDeltaView(true, null)).not.toThrow();
  });
});

describe("toRatesPerSecond", () => {
  it("divides each difference by the seconds between its two samples", () => {
    expect(toRatesPerSecond([0, 10, 20], [0, 50, 150])).toEqual([null, 5, 10]);
  });

  it("keeps a mixed cadence comparable (the log-to-live step is not a cliff)", () => {
    // 600 over 600 s from the log, then 10 over 5 s from the live poll.
    expect(toRatesPerSecond([0, 600, 605], [0, 600, 610])).toEqual([null, 1, 2]);
  });

  it("treats a decrease as a gap, never a negative rate", () => {
    expect(toRatesPerSecond([0, 5, 10], [100, 150, 50])).toEqual([null, 10, null]);
  });

  it("treats a non-finite value and a non-positive time step as gaps", () => {
    expect(toRatesPerSecond([0, 5, 10, 10], [0, Number.NaN as never, 20, 30])).toEqual([null, null, null, null]);
    expect(toRatesPerSecond([0, 0, 5], [0, 10, 20])).toEqual([null, null, 2]);
  });

  it("passes input nulls through as gaps on both sides of the hole", () => {
    expect(toRatesPerSecond([0, 5, 10, 15], [0, null, 20, 50])).toEqual([null, null, null, 6]);
  });
});

describe("formatRate", () => {
  it("suffixes the unit and keeps at most three significant digits below 100", () => {
    expect(formatRate(2.5)).toBe("2.5/s");
    expect(formatRate(9.876)).toBe("9.88/s");
    expect(formatRate(0.01234)).toBe("0.0123/s");
    expect(formatRate(50)).toBe("50/s");
  });

  it("uses no decimals at or above 100", () => {
    expect(formatRate(100.4)).toBe("100/s");
    expect(formatRate(342)).toBe("342/s");
  });
});

describe("gauges are never differenced", () => {
  it("seriesForDisplay returns raw values for a gauge even when the view is on", () => {
    expect(seriesForDisplay([0, 5, 10], [100, 50, 75], true, true)).toEqual([100, 50, 75]);
  });

  it("seriesForDisplay rates a counter against the sample timestamps when the view is on", () => {
    expect(seriesForDisplay([0, 5, 10], [100, 150, 175], true)).toEqual([null, 10, 5]);
  });

  it("graphTitle marks the rate view and leaves a gauge unmarked", () => {
    expect(graphTitle("inflight", true, true)).toBe("inflight");
    expect(graphTitle("inflight", false, true)).toBe("inflight");
    expect(graphTitle("num_flushes", true)).toBe("num_flushes (per second)");
    expect(graphTitle("num_flushes", false)).toBe("num_flushes");
  });
});

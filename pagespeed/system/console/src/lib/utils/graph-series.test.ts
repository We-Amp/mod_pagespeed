import { describe, it, expect } from "vitest";
import {
  DELTA_VIEW_STORAGE_KEY,
  graphTitle,
  latestForDisplay,
  loadDeltaView,
  saveDeltaView,
  seriesForDisplay,
  toPerIntervalDeltas,
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

describe("toPerIntervalDeltas", () => {
  it("differences consecutive samples of a cumulative counter", () => {
    expect(toPerIntervalDeltas([10, 13, 13, 20])).toEqual([null, 3, 0, 7]);
  });

  it("gaps the first sample, which has no predecessor", () => {
    expect(toPerIntervalDeltas([42])).toEqual([null]);
  });

  it("emits a gap where the counter was reset by a restart", () => {
    // Cumulative counter drops back to (near) zero at the restart.
    expect(toPerIntervalDeltas([100, 140, 5, 9])).toEqual([null, 40, null, 4]);
  });

  it("never emits a negative spike, however large the reset", () => {
    const deltas = toPerIntervalDeltas([1_000_000, 1, 4]);
    expect(deltas).toEqual([null, null, 3]);
    expect(deltas.some((d) => d !== null && d < 0)).toBe(false);
  });

  it("resumes normal deltas on the sample after a reset", () => {
    expect(toPerIntervalDeltas([50, 0, 7, 11])).toEqual([null, null, 7, 4]);
  });

  it("keeps the result aligned with the sample timestamps", () => {
    const values = [1, 2, 3, 2, 5];
    expect(toPerIntervalDeltas(values)).toHaveLength(values.length);
  });

  it("returns an empty series for an empty input", () => {
    expect(toPerIntervalDeltas([])).toEqual([]);
  });

  it("gaps differences involving a non-finite sample", () => {
    expect(toPerIntervalDeltas([1, NaN, 5, 8])).toEqual([null, null, null, 3]);
  });
});

describe("seriesForDisplay", () => {
  it("plots the cumulative values unchanged when the view is off", () => {
    expect(seriesForDisplay([10, 13, 5], false)).toEqual([10, 13, 5]);
  });

  it("plots per-interval deltas when the view is on", () => {
    expect(seriesForDisplay([10, 13, 5], true)).toEqual([null, 3, null]);
  });

  it("does not mutate the input series", () => {
    const values = [1, 2, 3];
    seriesForDisplay(values, true);
    seriesForDisplay(values, false);
    expect(values).toEqual([1, 2, 3]);
  });
});

describe("latestForDisplay", () => {
  it("reports the final sample when it has a value", () => {
    expect(latestForDisplay([null, 3, 0, 7])).toBe(7);
  });

  it("reports nothing when the final sample is a gap", () => {
    // The range ends on a counter reset: there is no value for that interval,
    // and the previous interval's 40 is not it.
    expect(latestForDisplay([null, 40, null])).toBeNull();
  });

  it("reports nothing for a series that is only a gap", () => {
    expect(latestForDisplay([null])).toBeNull();
  });

  it("reports nothing for an empty series", () => {
    expect(latestForDisplay([])).toBeNull();
  });

  it("reports a final zero rather than treating it as missing", () => {
    expect(latestForDisplay([null, 5, 0])).toBe(0);
  });

  it("reports the last cumulative value when the view is off", () => {
    expect(latestForDisplay(seriesForDisplay([10, 13, 20], false))).toBe(20);
  });

  it("reports nothing when a restart ends the range in delta view", () => {
    expect(latestForDisplay(seriesForDisplay([100, 140, 5], true))).toBeNull();
  });
});

describe("graphTitle", () => {
  it("marks the title when the per-interval view is on", () => {
    expect(graphTitle("http.requests", true)).toBe("http.requests (per interval)");
  });

  it("leaves the title alone when the view is off", () => {
    expect(graphTitle("http.requests", false)).toBe("http.requests");
  });
});

describe("delta-view persistence", () => {
  it("defaults to off when nothing was ever stored", () => {
    expect(loadDeltaView(fakeStorage())).toBe(false);
  });

  it("round-trips the toggle through storage", () => {
    const storage = fakeStorage();
    saveDeltaView(true, storage);
    expect(storage.getItem(DELTA_VIEW_STORAGE_KEY)).toBe("1");
    expect(loadDeltaView(storage)).toBe(true);

    saveDeltaView(false, storage);
    expect(loadDeltaView(storage)).toBe(false);
  });

  it("treats an unrecognised stored value as off", () => {
    expect(loadDeltaView(fakeStorage({ [DELTA_VIEW_STORAGE_KEY]: "yes" }))).toBe(false);
    expect(loadDeltaView(fakeStorage({ [DELTA_VIEW_STORAGE_KEY]: "" }))).toBe(false);
  });

  it("defaults to off when storage is absent", () => {
    expect(loadDeltaView(null)).toBe(false);
  });

  it("defaults to off outside a browser, where there is no localStorage", () => {
    // No argument, so the default-storage path runs. Under the node test
    // environment there is no `localStorage`, which is the same shape as a
    // server-rendered or embedded context.
    expect(loadDeltaView()).toBe(false);
  });

  it("stays off, and does not throw, when storage access throws", () => {
    expect(loadDeltaView(throwingStorage())).toBe(false);
  });

  it("does not throw when persisting into unavailable storage", () => {
    expect(() => saveDeltaView(true, throwingStorage())).not.toThrow();
    expect(() => saveDeltaView(true, null)).not.toThrow();
  });
});

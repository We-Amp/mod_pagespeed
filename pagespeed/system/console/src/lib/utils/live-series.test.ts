// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { beforeEach, describe, expect, it } from "vitest";
import { LIVE_SERIES_CAPACITY, LiveSeriesStore, pushSample } from "./live-series";

describe("LiveSeriesStore", () => {
  let store: LiveSeriesStore;
  beforeEach(() => {
    store = new LiveSeriesStore();
  });

  it("registers a group once and accepts the same registration again", () => {
    store.register("g", ["a", "b"]);
    expect(() => store.register("g", ["a", "b"])).not.toThrow();
    expect(store.names()).toEqual(["g"]);
  });

  it("rejects a conflicting re-registration", () => {
    store.register("g", ["a"]);
    expect(() => store.register("g", ["a", "b"])).toThrow(/already registered/);
    expect(() => store.register("g", ["b"])).toThrow(/already registered/);
  });

  it("rejects a push to an unregistered group", () => {
    expect(() => store.push("nope", 1, [1])).toThrow(/not registered/);
  });

  it("returns null for unknown or empty groups", () => {
    expect(store.get("nope")).toBeNull();
    store.register("g", ["a"]);
    expect(store.get("g")).toBeNull();
  });

  it("keeps series aligned on shared timestamps, nulls included", () => {
    store.register("g", ["a", "b"]);
    store.push("g", 100, [1, null]);
    store.push("g", 105, [2, 20]);
    const view = store.get("g");
    expect(view).not.toBeNull();
    expect(view!.names).toEqual(["a", "b"]);
    expect(view!.timestamps).toEqual([100, 105]);
    expect(view!.series).toEqual([
      [1, 2],
      [null, 20],
    ]);
  });

  it("evicts the oldest sample past the capacity", () => {
    store.register("g", ["a"]);
    for (let i = 0; i < LIVE_SERIES_CAPACITY + 3; i++) store.push("g", i * 5, [i]);
    const view = store.get("g")!;
    expect(view.timestamps.length).toBe(LIVE_SERIES_CAPACITY);
    expect(view.timestamps[0]).toBe(3 * 5);
  });

  it("reports the newest timestamp without copying the window", () => {
    store.register("g", ["a"]);
    expect(store.newestTimestamp("g")).toBeNull();
    store.push("g", 100, [1]);
    store.push("g", 105, [2]);
    expect(store.newestTimestamp("g")).toBe(105);
  });

  it("keeps reporting the newest timestamp after the buffer wraps", () => {
    store.register("g", ["a"]);
    for (let i = 0; i < LIVE_SERIES_CAPACITY + 3; i++) store.push("g", i * 5, [i]);
    expect(store.newestTimestamp("g")).toBe((LIVE_SERIES_CAPACITY + 2) * 5);
  });

  it("reports null for an unknown group", () => {
    expect(store.newestTimestamp("nope")).toBeNull();
  });

  it("clear() drops every group", () => {
    store.register("g", ["a"]);
    store.push("g", 1, [1]);
    store.clear();
    expect(store.names()).toEqual([]);
    expect(store.get("g")).toBeNull();
    // Re-registering after a clear is a fresh start, not a conflict.
    store.register("g", ["x", "y"]);
    store.push("g", 1, [1, 2]);
    expect(store.get("g")!.names).toEqual(["x", "y"]);
  });
});

describe("pushSample", () => {
  it("breaks the line at a paused-tab gap with a null row", () => {
    const store = new LiveSeriesStore();
    store.register("g", ["a", "b"]);
    pushSample(store, "g", 100, [1, 2], 5);
    pushSample(store, "g", 105, [3, 4], 5);
    // 105 -> 200 is a 19 s gap: a null row lands at 110 first.
    pushSample(store, "g", 200, [5, 6], 5);
    const view = store.get("g");
    expect(view?.timestamps).toEqual([100, 105, 110, 200]);
    expect(view?.series[0]).toEqual([1, 3, null, 5]);
    expect(view?.series[1]).toEqual([2, 4, null, 6]);
  });

  it("adds no null row on ordinary spacing or a single miss", () => {
    const store = new LiveSeriesStore();
    store.register("g", ["a"]);
    pushSample(store, "g", 100, [1], 5);
    pushSample(store, "g", 105, [2], 5);
    pushSample(store, "g", 115, [3], 5); // 10 s = 2x interval: no break
    const view = store.get("g");
    expect(view?.timestamps).toEqual([100, 105, 115]);
    expect(view?.series[0]).toEqual([1, 2, 3]);
  });

  it("seeds a fresh series without a null row", () => {
    const store = new LiveSeriesStore();
    store.register("g", ["a"]);
    pushSample(store, "g", 100, [1], 5);
    expect(store.get("g")?.series[0]).toEqual([1]);
  });
});

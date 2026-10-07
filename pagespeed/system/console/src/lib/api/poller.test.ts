// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ApiError } from "./client";
import {
  DEFAULT_BUSY_RETRY_MS,
  Poller,
  PollerRegistry,
  backoffDelay,
  isBusyAnswer,
  type PollerOptions,
  type VisibilitySource,
} from "./poller";

const T = 5000;

function fakeVisibility(hidden = false) {
  let isHidden = hidden;
  const subscribers = new Set<() => void>();
  const source: VisibilitySource & { set(h: boolean): void; listeners(): number } = {
    hidden: () => isHidden,
    subscribe(fn) {
      subscribers.add(fn);
      return () => {
        subscribers.delete(fn);
      };
    },
    set(h) {
      isHidden = h;
      for (const fn of subscribers) fn();
    },
    listeners: () => subscribers.size,
  };
  return source;
}

function make<V>(fetcher: () => Promise<V>, extra: Partial<PollerOptions> = {}): Poller<V> {
  return new Poller(fetcher, { intervalMs: T, visibility: null, ...extra });
}

/** A fetcher whose every call stays pending until the test resolves it. */
function pending<V>() {
  const resolvers: Array<(v: V) => void> = [];
  const fetcher = vi.fn(() => new Promise<V>((r) => resolvers.push(r)));
  return { fetcher, resolve: (i: number, v: V) => resolvers[i](v) };
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.setSystemTime(0);
});
afterEach(() => {
  vi.useRealTimers();
});

describe("backoffDelay", () => {
  it("waits the interval after the first failure, then doubles, capped at a minute", () => {
    expect([1, 2, 3, 4, 5, 6, 12].map((f) => backoffDelay(5000, f))).toEqual([
      5000, 10000, 20000, 40000, 60000, 60000, 60000,
    ]);
  });
  it("never waits less than the interval, even when the interval is above the cap", () => {
    expect(backoffDelay(90_000, 3)).toBe(90_000);
    expect(backoffDelay(5000, 0)).toBe(5000);
  });
});

describe("isBusyAnswer", () => {
  it("is true only for an HTTP 429 answer", () => {
    expect(isBusyAnswer(new ApiError(429, "a request for this daemon endpoint is already in flight"))).toBe(true);
    expect(isBusyAnswer(new ApiError(502, "daemon_unreachable"))).toBe(false);
    expect(isBusyAnswer(new Error("HTTP 429"))).toBe(false);
  });
});

describe("Poller", () => {
  it("fetches at start and then once per interval", async () => {
    const fetcher = vi.fn(async () => 1);
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(1);
    expect(p.snapshot).toMatchObject({ data: 1, error: null, loading: false, failures: 0, autoRefresh: true });
    await vi.advanceTimersByTimeAsync(T);
    expect(fetcher).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(T);
    expect(fetcher).toHaveBeenCalledTimes(3);
    p.dispose();
  });

  it("never overlaps: the next request waits for the previous one to settle", async () => {
    const { fetcher, resolve } = pending<number>();
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(12_000);
    expect(fetcher).toHaveBeenCalledTimes(1);
    resolve(0, 7);
    await vi.advanceTimersByTimeAsync(0);
    expect(p.snapshot.data).toBe(7);
    await vi.advanceTimersByTimeAsync(T - 1);
    expect(fetcher).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(fetcher).toHaveBeenCalledTimes(2);
    p.dispose();
  });

  it("refresh() during a request returns that request instead of starting another", async () => {
    const { fetcher, resolve } = pending<number>();
    const p = make(fetcher);
    p.start();
    const a = p.refresh();
    const b = p.refresh();
    expect(a).toBe(b);
    expect(fetcher).toHaveBeenCalledTimes(1);
    resolve(0, 3);
    await a;
    expect(p.snapshot.data).toBe(3);
    p.dispose();
  });

  it("a failing fetcher backs off and a success resets the interval", async () => {
    const calls: number[] = [];
    let failing = true;
    const fetcher = vi.fn(async () => {
      calls.push(Date.now());
      if (failing) throw new Error("down");
      return 1;
    });
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(195_000);
    expect(calls).toEqual([0, 5000, 15000, 35000, 75000, 135000, 195000]);
    expect(p.snapshot.failures).toBe(7);
    expect(p.snapshot.error?.message).toBe("down");
    expect(p.snapshot.nextAt).toBe(255_000);
    failing = false;
    await vi.advanceTimersByTimeAsync(60_000);
    expect(calls.at(-1)).toBe(255_000);
    expect(p.snapshot).toMatchObject({ failures: 0, error: null, data: 1 });
    await vi.advanceTimersByTimeAsync(T);
    expect(calls.at(-1)).toBe(260_000);
    p.dispose();
  });

  it("a failure keeps the last data on screen", async () => {
    const fetcher = vi.fn<() => Promise<number>>().mockResolvedValueOnce(5).mockRejectedValue(new Error("x"));
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(T);
    expect(p.snapshot).toMatchObject({ data: 5, failures: 1, loading: false });
    expect(p.snapshot.error?.message).toBe("x");
    p.dispose();
  });

  it("busy answers keep the last view and do not count as failures", async () => {
    const fetcher = vi
      .fn<() => Promise<number>>()
      .mockResolvedValueOnce(5)
      .mockRejectedValueOnce(new ApiError(429, "busy"))
      .mockResolvedValue(6);
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(T);
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(p.snapshot).toMatchObject({ data: 5, error: null, failures: 0, loading: false });
    await vi.advanceTimersByTimeAsync(T - 1);
    expect(fetcher).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(1);
    expect(fetcher).toHaveBeenCalledTimes(3);
    expect(p.snapshot.data).toBe(6);
    p.dispose();
  });

  it("a busy first answer keeps loading and retries after a second", async () => {
    const fetcher = vi.fn<() => Promise<number>>().mockRejectedValueOnce(new ApiError(429, "busy")).mockResolvedValue(1);
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(p.snapshot).toMatchObject({ data: null, error: null, loading: true });
    await vi.advanceTimersByTimeAsync(DEFAULT_BUSY_RETRY_MS);
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(p.snapshot).toMatchObject({ data: 1, loading: false });
    p.dispose();
  });

  it("pauses while the tab is hidden and refreshes at once when it is shown", async () => {
    const vis = fakeVisibility(false);
    const fetcher = vi.fn(async () => 1);
    const p = make(fetcher, { visibility: vis });
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    vis.set(true);
    expect(p.snapshot).toMatchObject({ paused: true, nextAt: null });
    await vi.advanceTimersByTimeAsync(10 * T);
    expect(fetcher).toHaveBeenCalledTimes(1);
    vis.set(false);
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(p.snapshot.paused).toBe(false);
    await vi.advanceTimersByTimeAsync(T);
    expect(fetcher).toHaveBeenCalledTimes(3);
    p.dispose();
  });

  it("a request in flight when the tab hides schedules nothing after it", async () => {
    const vis = fakeVisibility(false);
    const { fetcher, resolve } = pending<number>();
    const p = make(fetcher, { visibility: vis });
    p.start();
    vis.set(true);
    resolve(0, 1);
    await vi.advanceTimersByTimeAsync(10 * T);
    expect(fetcher).toHaveBeenCalledTimes(1);
    expect(p.snapshot.data).toBe(1);
    vis.set(false);
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(2);
    p.dispose();
  });

  it("started in a hidden tab: nothing until the tab is shown", async () => {
    const vis = fakeVisibility(true);
    const fetcher = vi.fn(async () => 1);
    const p = make(fetcher, { visibility: vis });
    p.start();
    await vi.advanceTimersByTimeAsync(3 * T);
    expect(fetcher).not.toHaveBeenCalled();
    expect(p.snapshot).toMatchObject({ loading: true, paused: true });
    vis.set(false);
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(1);
    p.dispose();
  });

  it("stop() ends polling, refresh() still works, start() resumes at once", async () => {
    const fetcher = vi.fn(async () => 1);
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    p.stop();
    expect(p.snapshot).toMatchObject({ autoRefresh: false, nextAt: null });
    await vi.advanceTimersByTimeAsync(3 * T);
    expect(fetcher).toHaveBeenCalledTimes(1);
    await p.refresh();
    expect(fetcher).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(3 * T);
    expect(fetcher).toHaveBeenCalledTimes(2);
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(3);
    await vi.advanceTimersByTimeAsync(T);
    expect(fetcher).toHaveBeenCalledTimes(4);
    p.dispose();
  });

  it("invalidate() drops the in-flight answer and fetches again", async () => {
    const { fetcher, resolve } = pending<string>();
    const p = make(fetcher);
    p.start();
    p.invalidate();
    expect(fetcher).toHaveBeenCalledTimes(1);
    resolve(0, "old range");
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(p.snapshot.data).toBeNull();
    resolve(1, "new range");
    await vi.advanceTimersByTimeAsync(0);
    expect(p.snapshot.data).toBe("new range");
    p.dispose();
  });

  it("invalidate() when idle fetches at once, even with auto-refresh paused", async () => {
    const fetcher = vi.fn(async () => 1);
    const p = make(fetcher);
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    p.stop();
    p.invalidate();
    await vi.advanceTimersByTimeAsync(0);
    expect(fetcher).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(3 * T);
    expect(fetcher).toHaveBeenCalledTimes(2);
    p.dispose();
  });

  it("dispose() stops everything, unsubscribes and ignores a late answer", async () => {
    const vis = fakeVisibility(false);
    const { fetcher, resolve } = pending<number>();
    const p = make(fetcher, { visibility: vis });
    p.start();
    p.dispose();
    expect(vis.listeners()).toBe(0);
    resolve(0, 1);
    await vi.advanceTimersByTimeAsync(3 * T);
    expect(fetcher).toHaveBeenCalledTimes(1);
    expect(p.snapshot.data).toBeNull();
  });

  it("listeners see each new state until they unsubscribe", async () => {
    const fetcher = vi.fn(async () => 1);
    const p = make(fetcher);
    const seen: Array<number | null> = [];
    const off = p.subscribe((s) => seen.push(s.data));
    p.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(seen).toContain(1);
    off();
    const n = seen.length;
    await vi.advanceTimersByTimeAsync(T);
    expect(seen.length).toBe(n);
    p.dispose();
  });
});

describe("PollerRegistry", () => {
  it("refreshAll asks every registered poller; unregistering removes one", async () => {
    const reg = new PollerRegistry();
    const a = { refresh: vi.fn(async () => {}) };
    const b = { refresh: vi.fn(async () => {}) };
    const offA = reg.add(a);
    reg.add(b);
    expect(reg.size).toBe(2);
    await reg.refreshAll();
    offA();
    await reg.refreshAll();
    expect(a.refresh).toHaveBeenCalledTimes(1);
    expect(b.refresh).toHaveBeenCalledTimes(2);
    expect(reg.size).toBe(1);
  });
});

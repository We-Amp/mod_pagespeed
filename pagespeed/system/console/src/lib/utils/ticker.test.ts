// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { VisibilitySource } from "$lib/api/poller";
import { Ticker } from "./ticker";

/** Manual timers: `advance` fires what is due, in order. */
function fakeTimers() {
  let now = 0;
  let nextId = 1;
  const pending = new Map<number, { at: number; fn: () => void }>();
  return {
    setTimeout(fn: () => void, ms: number): unknown {
      const id = nextId++;
      pending.set(id, { at: now + ms, fn });
      return id;
    },
    clearTimeout(handle: unknown): void {
      pending.delete(handle as number);
    },
    advance(ms: number): void {
      const end = now + ms;
      for (;;) {
        const due = [...pending.entries()].filter(([, t]) => t.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
        if (due === undefined) break;
        pending.delete(due[0]);
        now = due[1].at;
        due[1].fn();
      }
      now = end;
    },
    get size(): number {
      return pending.size;
    },
  };
}

function fakeVisibility(initiallyHidden = false) {
  let hidden = initiallyHidden;
  const listeners = new Set<() => void>();
  const source: VisibilitySource = {
    hidden: () => hidden,
    subscribe(onChange) {
      listeners.add(onChange);
      return () => listeners.delete(onChange);
    },
  };
  return {
    source,
    set(value: boolean): void {
      hidden = value;
      for (const fn of [...listeners]) fn();
    },
    get listeners(): number {
      return listeners.size;
    },
  };
}

describe("Ticker", () => {
  it("ticks once a period, one timer at a time", () => {
    const timers = fakeTimers();
    const ticker = new Ticker(1000, timers, null);
    let ticks = 0;
    ticker.start(() => ticks++);
    expect(timers.size).toBe(1);
    timers.advance(999);
    expect(ticks).toBe(0);
    timers.advance(1);
    expect(ticks).toBe(1);
    timers.advance(5000);
    expect(ticks).toBe(6);
    expect(timers.size).toBe(1);
  });

  it("stops: no timer and no listener left behind", () => {
    const timers = fakeTimers();
    const visibility = fakeVisibility();
    const ticker = new Ticker(1000, timers, visibility.source);
    let ticks = 0;
    const stop = ticker.start(() => ticks++);
    expect(ticker.running).toBe(true);
    expect(visibility.listeners).toBe(1);
    stop();
    expect(ticker.running).toBe(false);
    expect(timers.size).toBe(0);
    expect(visibility.listeners).toBe(0);
    timers.advance(10_000);
    expect(ticks).toBe(0);
  });

  it("does nothing in a hidden tab and ticks at once when it is shown", () => {
    const timers = fakeTimers();
    const visibility = fakeVisibility();
    const ticker = new Ticker(1000, timers, visibility.source);
    let ticks = 0;
    ticker.start(() => ticks++);
    timers.advance(2000);
    expect(ticks).toBe(2);

    visibility.set(true);
    expect(timers.size).toBe(0);
    timers.advance(60_000);
    expect(ticks).toBe(2);

    visibility.set(false);
    expect(ticks).toBe(3);
    expect(timers.size).toBe(1);
    timers.advance(1000);
    expect(ticks).toBe(4);
  });

  it("started in a hidden tab, waits until the tab is shown", () => {
    const timers = fakeTimers();
    const visibility = fakeVisibility(true);
    const ticker = new Ticker(1000, timers, visibility.source);
    let ticks = 0;
    ticker.start(() => ticks++);
    expect(timers.size).toBe(0);
    visibility.set(false);
    expect(ticks).toBe(1);
    expect(timers.size).toBe(1);
  });

  it("a repeated visibility event does not double the chain", () => {
    const timers = fakeTimers();
    const visibility = fakeVisibility();
    const ticker = new Ticker(1000, timers, visibility.source);
    let ticks = 0;
    ticker.start(() => ticks++);
    visibility.set(false);
    visibility.set(false);
    expect(ticks).toBe(0);
    expect(timers.size).toBe(1);
  });

  it("a tick that stops the ticker schedules nothing more", () => {
    const timers = fakeTimers();
    const ticker = new Ticker(1000, timers, null);
    let ticks = 0;
    ticker.start(() => {
      ticks++;
      ticker.stop();
    });
    timers.advance(5000);
    expect(ticks).toBe(1);
    expect(timers.size).toBe(0);
  });
});

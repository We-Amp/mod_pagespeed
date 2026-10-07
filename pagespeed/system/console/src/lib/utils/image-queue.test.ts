// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  PREVIEW_CONCURRENCY,
  RETRY_DELAYS_MS,
  createLimiter,
  loadWithRetry,
} from "./image-queue";

describe("createLimiter", () => {
  it("matches the module's two content slots", () => {
    expect(PREVIEW_CONCURRENCY).toBe(2);
  });
  it("admits two at once, queues the rest FIFO, and hands a released slot on", async () => {
    const limiter = createLimiter(2);
    const order: string[] = [];
    const r1 = await limiter.acquire();
    const r2 = await limiter.acquire();
    const third = limiter.acquire().then((r) => (order.push("third"), r));
    const fourth = limiter.acquire().then((r) => (order.push("fourth"), r));
    expect(limiter.active).toBe(2);
    expect(limiter.waiting).toBe(2);
    r1();
    const r3 = await third;
    expect(order).toEqual(["third"]);
    expect(limiter.active).toBe(2);
    r1(); // releasing twice is a no-op
    expect(limiter.waiting).toBe(1);
    r2();
    const r4 = await fourth;
    expect(order).toEqual(["third", "fourth"]);
    r3();
    r4();
    expect(limiter.active).toBe(0);
    expect(limiter.waiting).toBe(0);
  });
});

describe("loadWithRetry", () => {
  const recordingSleep = () => {
    const waits: number[] = [];
    return { waits, sleep: async (ms: number) => void waits.push(ms) };
  };

  it("loads at once without probing", async () => {
    const { waits, sleep } = recordingSleep();
    let probes = 0;
    const outcome = await loadWithRetry(async () => true, async () => (probes++, 200), sleep);
    expect(outcome).toBe("loaded");
    expect(probes).toBe(0);
    expect(waits).toEqual([]);
  });
  it("retries a busy (429) answer after 1 s and then loads", async () => {
    const { waits, sleep } = recordingSleep();
    let attempts = 0;
    const outcome = await loadWithRetry(async () => ++attempts === 2, async () => 429, sleep);
    expect(outcome).toBe("loaded");
    expect(attempts).toBe(2);
    expect(waits).toEqual([1000]);
  });
  it("gives up as busy after three retries at 1 s / 2 s / 4 s", async () => {
    const { waits, sleep } = recordingSleep();
    let attempts = 0;
    const outcome = await loadWithRetry(async () => (attempts++, false), async () => 429, sleep);
    expect(outcome).toBe("busy");
    expect(attempts).toBe(4);
    expect(waits).toEqual([...RETRY_DELAYS_MS]);
    expect(RETRY_DELAYS_MS).toEqual([1000, 2000, 4000]);
  });
  it("does not retry a permanent failure", async () => {
    for (const status of [415, 404, 502, 0]) {
      const { waits, sleep } = recordingSleep();
      let attempts = 0;
      const outcome = await loadWithRetry(async () => (attempts++, false), async () => status, sleep);
      expect(outcome, String(status)).toBe("failed");
      expect(attempts).toBe(1);
      expect(waits).toEqual([]);
    }
  });
  it("stops when cancelled", async () => {
    const { sleep } = recordingSleep();
    let cancelled = false;
    const outcome = await loadWithRetry(
      async () => ((cancelled = true), false),
      async () => 429,
      sleep,
      () => cancelled,
    );
    expect(outcome).toBe("cancelled");
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Loading cached-variant images without tripping the module's content
 * slots: at most PREVIEW_CONCURRENCY images load at once (one limiter per
 * page), and an image whose load failed because the server was busy (a
 * 429, learned from a headers-only probe — an <img> cannot see a status)
 * is retried after 1 s, 2 s and 4 s before the page says "busy".
 */

/** The module admits two concurrent content reads per process. */
export const PREVIEW_CONCURRENCY = 2;
/** Waits before each retry of a busy image. */
export const RETRY_DELAYS_MS: readonly number[] = [1000, 2000, 4000];

export interface Limiter {
  /** Resolves with a release function once a slot is free (FIFO). */
  acquire(): Promise<() => void>;
  readonly active: number;
  readonly waiting: number;
}

export function createLimiter(max: number): Limiter {
  let active = 0;
  const queue: Array<() => void> = [];
  const releaseSlot = () => {
    const next = queue.shift();
    if (next !== undefined) {
      next(); // the slot passes straight to the next waiter
    } else {
      active -= 1;
    }
  };
  return {
    acquire() {
      return new Promise<() => void>((resolve) => {
        const grant = () => {
          let released = false;
          resolve(() => {
            if (released) return;
            released = true;
            releaseSlot();
          });
        };
        if (active < max) {
          active += 1;
          grant();
        } else {
          queue.push(grant);
        }
      });
    },
    get active() {
      return active;
    },
    get waiting() {
      return queue.length;
    },
  };
}

export type PreviewOutcome = "loaded" | "busy" | "failed" | "cancelled";

export async function loadWithRetry(
  attempt: () => Promise<boolean>,
  probe: () => Promise<number>,
  sleep: (ms: number) => Promise<void>,
  cancelled: () => boolean = () => false,
): Promise<PreviewOutcome> {
  for (let retry = 0; ; retry++) {
    if (cancelled()) return "cancelled";
    if (await attempt()) return "loaded";
    if (cancelled()) return "cancelled";
    const status = await probe();
    if (status !== 429) return "failed";
    if (retry >= RETRY_DELAYS_MS.length) return "busy";
    await sleep(RETRY_DELAYS_MS[retry]);
  }
}

/** A plain timer-based wait (setTimeout; the console runs no setInterval). */
export function realSleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

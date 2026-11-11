// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A filter's result count, told to a screen reader once the typing stops:
// the visible count changes with every key, which a live region would read
// out letter by letter. The page puts the published text in a polite,
// visually hidden status; polling never calls this, so a refresh stays
// silent.

/** How long the filter must be quiet before its count is read out. */
export const FILTER_STATUS_DELAY_MS = 600;

interface Clock {
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(id: unknown): void;
}

const GLOBAL_CLOCK: Clock = {
  setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
  clearTimeout: (id) => globalThis.clearTimeout(id as ReturnType<typeof setTimeout>),
};

/**
 * Returns `schedule(read)`: each call restarts the wait, and when the wait
 * ends, `publish(read())` runs once, with the count as it is then.
 */
export function settledCount(
  publish: (text: string) => void,
  delayMs = FILTER_STATUS_DELAY_MS,
  clock: Clock = GLOBAL_CLOCK,
): (read: () => string) => void {
  let timer: unknown;
  return (read) => {
    if (timer !== undefined) clock.clearTimeout(timer);
    timer = clock.setTimeout(() => {
      timer = undefined;
      publish(read());
    }, delayMs);
  };
}

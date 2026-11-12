// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { FILTER_STATUS_DELAY_MS, settledCount } from "./filter-status";

describe("settledCount", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("publishes once, after the typing stops, with the count as it is then", () => {
    const published: string[] = [];
    const schedule = settledCount((t) => published.push(t));
    let count = 10;
    schedule(() => `${count} of 10`);
    vi.advanceTimersByTime(FILTER_STATUS_DELAY_MS - 1);
    count = 4;
    schedule(() => `${count} of 10`);
    vi.advanceTimersByTime(FILTER_STATUS_DELAY_MS - 1);
    count = 2;
    schedule(() => `${count} of 10`);
    expect(published).toEqual([]);
    count = 1;
    vi.advanceTimersByTime(FILTER_STATUS_DELAY_MS);
    expect(published).toEqual(["1 of 10"]);
    vi.advanceTimersByTime(10 * FILTER_STATUS_DELAY_MS);
    expect(published).toEqual(["1 of 10"]);
  });

  it("publishes again for a later change", () => {
    const published: string[] = [];
    const schedule = settledCount((t) => published.push(t), 100);
    schedule(() => "3 of 9");
    vi.advanceTimersByTime(100);
    schedule(() => "9 of 9");
    vi.advanceTimersByTime(100);
    expect(published).toEqual(["3 of 9", "9 of 9"]);
  });
});

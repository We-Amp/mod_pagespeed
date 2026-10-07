// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { RingBuffer } from "./ring-buffer";

describe("RingBuffer", () => {
  it("defaults to 720 samples (one hour at a 5 s poll)", () => {
    expect(new RingBuffer(1).capacity).toBe(720);
  });

  it("rejects non-positive sizes", () => {
    expect(() => new RingBuffer(0)).toThrow(RangeError);
    expect(() => new RingBuffer(1, 0)).toThrow(RangeError);
  });

  it("reads back what was pushed, oldest first", () => {
    const rb = new RingBuffer(2, 10);
    rb.push(100, [1, 10]);
    rb.push(200, [2, 20]);
    rb.push(300, [3, 30]);
    expect(rb.size).toBe(3);
    expect(rb.timestamps).toEqual([100, 200, 300]);
    expect(rb.series).toEqual([
      [1, 2, 3],
      [10, 20, 30],
    ]);
  });

  it("overwrites the oldest sample once full", () => {
    const rb = new RingBuffer(1, 3);
    for (let i = 0; i < 5; i++) rb.push(i, [i * 10]);
    expect(rb.size).toBe(3);
    expect(rb.timestamps).toEqual([2, 3, 4]);
    expect(rb.series).toEqual([[20, 30, 40]]);
  });

  it("wraps repeatedly without losing order", () => {
    const rb = new RingBuffer(1, 4);
    for (let i = 0; i < 11; i++) rb.push(i, [i]);
    expect(rb.timestamps).toEqual([7, 8, 9, 10]);
    rb.push(11, [11]);
    expect(rb.timestamps).toEqual([8, 9, 10, 11]);
  });

  it("keeps null gaps as null", () => {
    const rb = new RingBuffer(2, 5);
    rb.push(1, [null, 5]);
    rb.push(2, [7, null]);
    expect(rb.series).toEqual([
      [null, 7],
      [5, null],
    ]);
  });

  it("alignedData is uPlot columnar: timestamps first", () => {
    const rb = new RingBuffer(2, 5);
    rb.push(1, [10, 100]);
    rb.push(2, [20, 200]);
    expect(rb.alignedData).toEqual([
      [1, 2],
      [10, 20],
      [100, 200],
    ]);
  });

  it("throws when the value count does not match the series count", () => {
    const rb = new RingBuffer(2, 5);
    expect(() => rb.push(1, [1])).toThrow(RangeError);
    expect(() => rb.push(1, [1, 2, 3])).toThrow(RangeError);
  });

  it("clear() empties it and resets the write position", () => {
    const rb = new RingBuffer(1, 3);
    rb.push(1, [1]);
    rb.push(2, [2]);
    rb.clear();
    expect(rb.size).toBe(0);
    expect(rb.timestamps).toEqual([]);
    rb.push(3, [3]);
    expect(rb.timestamps).toEqual([3]);
  });

  it("holds a full hour of 5 s samples for each series", () => {
    const rb = new RingBuffer(3, 720);
    for (let i = 0; i < 725; i++) rb.push(1000 + i * 5, [i, null, i * 2]);
    expect(rb.size).toBe(720);
    expect(rb.timestamps[0]).toBe(1000 + 5 * 5);
    expect(rb.series[2][719]).toBe(724 * 2);
  });
});

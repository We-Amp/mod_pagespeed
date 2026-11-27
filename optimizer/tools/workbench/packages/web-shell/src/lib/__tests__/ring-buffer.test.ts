// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { RingBuffer } from '../ring-buffer';

describe('RingBuffer', () => {
  it('starts empty', () => {
    const rb = new RingBuffer(2, 5);
    expect(rb.size).toBe(0);
    expect(rb.capacity).toBe(5);
    expect(rb.seriesCount).toBe(2);
    expect(rb.timestamps).toEqual([]);
    expect(rb.series).toEqual([[], []]);
  });

  it('rejects invalid construction', () => {
    expect(() => new RingBuffer(0)).toThrow('seriesCount must be >= 1');
    expect(() => new RingBuffer(1, 0)).toThrow('capacity must be >= 1');
  });

  it('rejects wrong number of values in push', () => {
    const rb = new RingBuffer(3, 5);
    expect(() => rb.push(1, [1, 2])).toThrow('Expected 3 values, got 2');
    expect(() => rb.push(1, [1, 2, 3, 4])).toThrow(
      'Expected 3 values, got 4',
    );
  });

  it('stores and retrieves data in order', () => {
    const rb = new RingBuffer(2, 5);
    rb.push(100, [10, 20]);
    rb.push(101, [11, 21]);
    rb.push(102, [12, 22]);

    expect(rb.size).toBe(3);
    expect(rb.timestamps).toEqual([100, 101, 102]);
    expect(rb.series).toEqual([
      [10, 11, 12],
      [20, 21, 22],
    ]);
  });

  it('wraps around when capacity is exceeded', () => {
    const rb = new RingBuffer(1, 3);
    rb.push(1, [10]);
    rb.push(2, [20]);
    rb.push(3, [30]);
    rb.push(4, [40]); // overwrites index 0
    rb.push(5, [50]); // overwrites index 1

    expect(rb.size).toBe(3);
    expect(rb.timestamps).toEqual([3, 4, 5]);
    expect(rb.series).toEqual([[30, 40, 50]]);
  });

  it('alignedData returns [timestamps, ...series]', () => {
    const rb = new RingBuffer(2, 5);
    rb.push(1, [10, 100]);
    rb.push(2, [20, 200]);

    const data = rb.alignedData;
    expect(data).toEqual([
      [1, 2],
      [10, 20],
      [100, 200],
    ]);
  });

  it('clear resets the buffer', () => {
    const rb = new RingBuffer(1, 5);
    rb.push(1, [10]);
    rb.push(2, [20]);
    expect(rb.size).toBe(2);

    rb.clear();
    expect(rb.size).toBe(0);
    expect(rb.timestamps).toEqual([]);
    expect(rb.series).toEqual([[]]);
  });

  it('works correctly at exactly capacity', () => {
    const rb = new RingBuffer(1, 3);
    rb.push(1, [10]);
    rb.push(2, [20]);
    rb.push(3, [30]);

    expect(rb.size).toBe(3);
    expect(rb.timestamps).toEqual([1, 2, 3]);
    expect(rb.series).toEqual([[10, 20, 30]]);
  });

  it('uses default capacity of 300', () => {
    const rb = new RingBuffer(1);
    expect(rb.capacity).toBe(300);
  });

  it('handles single-element capacity', () => {
    const rb = new RingBuffer(2, 1);
    rb.push(1, [10, 20]);
    expect(rb.timestamps).toEqual([1]);

    rb.push(2, [30, 40]);
    expect(rb.size).toBe(1);
    expect(rb.timestamps).toEqual([2]);
    expect(rb.series).toEqual([[30], [40]]);
  });

  it('returns new arrays each call (safe for external mutation)', () => {
    const rb = new RingBuffer(1, 5);
    rb.push(1, [10]);
    const t1 = rb.timestamps;
    const t2 = rb.timestamps;
    expect(t1).toEqual(t2);
    expect(t1).not.toBe(t2); // different array instance
  });

  it('handles heavy wrap-around correctly', () => {
    const rb = new RingBuffer(1, 4);
    for (let i = 0; i < 100; i++) {
      rb.push(i, [i * 10]);
    }

    expect(rb.size).toBe(4);
    // Should contain the last 4 entries in order
    expect(rb.timestamps).toEqual([96, 97, 98, 99]);
    expect(rb.series).toEqual([[960, 970, 980, 990]]);
  });
});

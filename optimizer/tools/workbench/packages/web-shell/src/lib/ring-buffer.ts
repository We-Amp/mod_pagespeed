// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Fixed-size circular buffer for time-series data.
 *
 * Stores timestamps and N parallel data series in aligned arrays.
 * When capacity is reached, the oldest entries are overwritten.
 *
 * Default capacity: 300 (5 minutes at 1-second polling interval).
 */
export class RingBuffer {
  private readonly _capacity: number;
  private readonly _seriesCount: number;
  private _timestamps: number[];
  private _series: number[][];
  private _head: number = 0;
  private _size: number = 0;

  constructor(seriesCount: number, capacity: number = 300) {
    if (seriesCount < 1) {
      throw new Error('seriesCount must be >= 1');
    }
    if (capacity < 1) {
      throw new Error('capacity must be >= 1');
    }
    this._capacity = capacity;
    this._seriesCount = seriesCount;
    this._timestamps = new Array(capacity);
    this._series = [];
    for (let i = 0; i < seriesCount; i++) {
      this._series.push(new Array(capacity));
    }
  }

  /** Number of data points currently stored. */
  get size(): number {
    return this._size;
  }

  /** Maximum number of data points. */
  get capacity(): number {
    return this._capacity;
  }

  /** Number of parallel data series. */
  get seriesCount(): number {
    return this._seriesCount;
  }

  /**
   * Push one data point across all series.
   *
   * @param timestamp  Unix timestamp (seconds) for this data point.
   * @param values     Array of values, one per series.  Length must equal seriesCount.
   */
  push(timestamp: number, values: number[]): void {
    if (values.length !== this._seriesCount) {
      throw new Error(
        `Expected ${this._seriesCount} values, got ${values.length}`,
      );
    }

    this._timestamps[this._head] = timestamp;
    for (let i = 0; i < this._seriesCount; i++) {
      this._series[i][this._head] = values[i];
    }

    this._head = (this._head + 1) % this._capacity;
    if (this._size < this._capacity) {
      this._size++;
    }
  }

  /**
   * Return timestamps in chronological order.
   * Returns a new array each call (safe to hand to uPlot).
   */
  get timestamps(): number[] {
    return this._readOrdered(this._timestamps);
  }

  /**
   * Return all series data in chronological order.
   * Returns an array of arrays: one sub-array per series.
   */
  get series(): number[][] {
    return this._series.map((s) => this._readOrdered(s));
  }

  /**
   * Return uPlot-compatible aligned data: [timestamps, ...series].
   */
  get alignedData(): number[][] {
    return [this.timestamps, ...this.series];
  }

  /** Clear all stored data. */
  clear(): void {
    this._head = 0;
    this._size = 0;
  }

  // -- Internal ---------------------------------------------------------------

  /** Read elements from the circular buffer in chronological order. */
  private _readOrdered(arr: number[]): number[] {
    if (this._size === 0) return [];

    if (this._size < this._capacity) {
      // Buffer not full yet; data starts at index 0.
      return arr.slice(0, this._size);
    }

    // Buffer is full; oldest entry is at _head.
    const tail = arr.slice(this._head, this._capacity);
    const head = arr.slice(0, this._head);
    return tail.concat(head);
  }
}

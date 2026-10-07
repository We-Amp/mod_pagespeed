// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * A fixed-capacity ring buffer of time-stamped samples for a fixed set of
 * series. Reads come out oldest first; once full, a push overwrites the
 * oldest sample. Timestamps are epoch seconds (uPlot's unit); a series
 * value of null is a gap. Memory only: nothing here is persisted.
 */
export class RingBuffer {
  readonly seriesCount: number;
  readonly capacity: number;
  private readonly ts: number[];
  private readonly vals: (number | null)[][];
  private count = 0;
  private next = 0;

  constructor(seriesCount: number, capacity = 720) {
    if (!Number.isInteger(seriesCount) || seriesCount < 1) {
      throw new RangeError("seriesCount must be a positive integer");
    }
    if (!Number.isInteger(capacity) || capacity < 1) {
      throw new RangeError("capacity must be a positive integer");
    }
    this.seriesCount = seriesCount;
    this.capacity = capacity;
    this.ts = new Array<number>(capacity);
    this.vals = Array.from({ length: seriesCount }, () => new Array<number | null>(capacity));
  }

  get size(): number {
    return this.count;
  }

  /** Append one sample: a timestamp plus exactly one value per series. */
  push(timestamp: number, values: (number | null)[]): void {
    if (values.length !== this.seriesCount) {
      throw new RangeError(`expected ${this.seriesCount} values, got ${values.length}`);
    }
    this.ts[this.next] = timestamp;
    for (let s = 0; s < this.seriesCount; s++) {
      this.vals[s][this.next] = values[s];
    }
    this.next = (this.next + 1) % this.capacity;
    if (this.count < this.capacity) this.count++;
  }

  /** Oldest-to-newest timestamps of the live window. */
  get timestamps(): number[] {
    return this.ordered((i) => this.ts[i]);
  }

  /** The newest sample's timestamp, or null while the buffer is empty. */
  get newestTimestamp(): number | null {
    return this.count === 0 ? null : this.ts[(this.next - 1 + this.capacity) % this.capacity];
  }

  /** Oldest-to-newest values per series, aligned with the timestamps. */
  get series(): (number | null)[][] {
    return this.vals.map((v) => this.ordered((i) => v[i]));
  }

  /** uPlot columnar data: [timestamps, ...seriesValues]. */
  get alignedData(): [number[], ...(number | null)[][]] {
    return [this.timestamps, ...this.series];
  }

  clear(): void {
    this.count = 0;
    this.next = 0;
  }

  private ordered<T>(read: (physicalIndex: number) => T): T[] {
    const out = new Array<T>(this.count);
    // Once the buffer has wrapped, the oldest live sample sits at `next`.
    const start = this.count === this.capacity ? this.next : 0;
    for (let i = 0; i < this.count; i++) {
      out[i] = read((start + i) % this.capacity);
    }
    return out;
  }
}

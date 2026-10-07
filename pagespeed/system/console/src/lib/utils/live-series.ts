// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The console's live time series, fed by the poll each page already runs.
 * One module-level store holds every group, so a series survives navigating
 * between console pages within one tab; a reload starts afresh and nothing
 * is persisted. Each group is a fixed set of series sharing one ring
 * buffer, so every series in a group is sampled at the same timestamps.
 *
 * The store is deliberately not reactive: pages read it inside a derivation
 * that also references the poll's data, so it is re-read after each
 * completed sample.
 */
import { RingBuffer } from "./ring-buffer";

/** One hour at the console's 5 s poll interval. */
export const LIVE_SERIES_CAPACITY = 720;

export interface LiveSeriesView {
  names: string[];
  /** Epoch seconds, oldest first. */
  timestamps: number[];
  series: (number | null)[][];
}

interface Group {
  names: string[];
  buffer: RingBuffer;
}

export class LiveSeriesStore {
  private readonly groups = new Map<string, Group>();

  /** Register a group. Idempotent for an identical registration. */
  register(group: string, seriesNames: readonly string[]): void {
    const existing = this.groups.get(group);
    if (existing) {
      const same =
        existing.names.length === seriesNames.length &&
        seriesNames.every((n, i) => n === existing.names[i]);
      if (!same) {
        throw new Error(`live series group "${group}" is already registered with different series`);
      }
      return;
    }
    this.groups.set(group, {
      names: [...seriesNames],
      buffer: new RingBuffer(seriesNames.length, LIVE_SERIES_CAPACITY),
    });
  }

  /** Append one sample to a registered group. */
  push(group: string, atSeconds: number, values: (number | null)[]): void {
    const g = this.groups.get(group);
    if (!g) throw new Error(`live series group "${group}" is not registered`);
    g.buffer.push(atSeconds, values);
  }

  /** The group's live window, or null when unknown or still empty. */
  get(group: string): LiveSeriesView | null {
    const g = this.groups.get(group);
    if (!g || g.buffer.size === 0) return null;
    return { names: [...g.names], timestamps: g.buffer.timestamps, series: g.buffer.series };
  }

  /** The group's newest sample's timestamp, or null when unknown or still empty. */
  newestTimestamp(group: string): number | null {
    return this.groups.get(group)?.buffer.newestTimestamp ?? null;
  }

  names(): string[] {
    return [...this.groups.keys()].sort();
  }

  clear(): void {
    this.groups.clear();
  }
}

/** The one store per tab. */
export const liveSeries = new LiveSeriesStore();

/** The Savings page's group: module rewrite savings and optimizer serve savings. */
export const SAVINGS_GROUP = "savings";
export const SAVINGS_SERIES = ["module.bytes_saved", "optimizer.bytes_saved"] as const;

/** Push one sample, breaking the line at a gap: when the newest retained
 *  sample is more than 2.5 intervals old (a paused tab, a long backoff), an
 *  all-null row lands at the next expected slot first, so a chart with
 *  spanGaps off shows the gap instead of bridging it with a straight line.
 *  Ordinary jitter — one missed poll — does not break the line. */
export function pushSample(
  store: LiveSeriesStore,
  group: string,
  atSeconds: number,
  values: ReadonlyArray<number | null>,
  intervalSeconds: number,
): void {
  const newest = store.newestTimestamp(group);
  if (newest !== null && atSeconds - newest > 2.5 * intervalSeconds) {
    store.push(
      group,
      newest + intervalSeconds,
      values.map(() => null),
    );
  }
  store.push(group, atSeconds, [...values]);
}

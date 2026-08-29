/**
 * Helpers for the graphs page's "per interval" (delta) view.
 *
 * The graphs endpoint returns the raw *cumulative* counter values sampled at
 * an even granularity. Every server restart recreates the statistics, so a
 * cumulative series drops back to zero at each restart and the curve reads as
 * a sawtooth rather than as traffic. Differencing consecutive samples turns
 * the series into "how much happened in this interval", and a negative
 * difference is the signature of a counter reset -- it is reported as a gap
 * (`null`) rather than as a large negative spike, so a restart shows up as a
 * break in the line instead of as data.
 */

/** localStorage key holding the graphs page's per-interval toggle. */
export const DELTA_VIEW_STORAGE_KEY = "pagespeed.graphs.perIntervalDeltas";

/** A plotted sample: a number, or `null` for "no value here" (a gap). */
export type Sample = number | null;

/**
 * Difference consecutive samples of a cumulative counter series.
 *
 * - The first sample has no predecessor, so it is a gap.
 * - A negative difference means the counter was reset (a restart), so it is a
 *   gap too -- never a negative spike.
 * - Non-finite input (missing/NaN) yields a gap for the affected difference.
 *
 * The result always has the same length as the input, so it stays aligned
 * with the sample timestamps.
 */
export function toPerIntervalDeltas(values: readonly number[]): Sample[] {
  const out: Sample[] = [];
  for (let i = 0; i < values.length; i++) {
    if (i === 0) {
      out.push(null);
      continue;
    }
    const prev = values[i - 1];
    const curr = values[i];
    if (!Number.isFinite(prev) || !Number.isFinite(curr)) {
      out.push(null);
      continue;
    }
    const delta = curr - prev;
    out.push(delta < 0 ? null : delta);
  }
  return out;
}

/**
 * The series to plot: per-interval deltas when the view is on, otherwise the
 * cumulative values unchanged.
 */
export function seriesForDisplay(
  values: readonly number[],
  deltaView: boolean,
): Sample[] {
  return deltaView ? toPerIntervalDeltas(values) : values.slice();
}

/**
 * The value to report as "Latest": the final sample of the displayed series,
 * or `null` when that sample is a gap. Falling back to the last non-null value
 * there would attribute a number to an interval that has none -- exactly the
 * misreading the gap exists to prevent.
 */
export function latestForDisplay(series: readonly Sample[]): number | null {
  if (series.length === 0) return null;
  const last = series[series.length - 1];
  return last !== null && Number.isFinite(last) ? last : null;
}

/** Chart title, marked when the per-interval view is on. */
export function graphTitle(name: string, deltaView: boolean): string {
  return deltaView ? `${name} (per interval)` : name;
}

/**
 * Read the persisted toggle. Defaults to off (the cumulative view stays the
 * default), and stays off if storage is unavailable or unreadable -- Safari's
 * private mode and "block all cookies" both make `localStorage` throw.
 */
export function loadDeltaView(storage: Storage | null = defaultStorage()): boolean {
  try {
    return storage?.getItem(DELTA_VIEW_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/** Persist the toggle. A storage failure must never break the page. */
export function saveDeltaView(
  enabled: boolean,
  storage: Storage | null = defaultStorage(),
): void {
  try {
    storage?.setItem(DELTA_VIEW_STORAGE_KEY, enabled ? "1" : "0");
  } catch {
    // Ignore: the toggle still works for this page view, it just won't persist.
  }
}

/** `localStorage` when the environment has one; merely touching it can throw. */
function defaultStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

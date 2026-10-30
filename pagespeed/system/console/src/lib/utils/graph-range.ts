// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { intervalLabel, type GraphsView } from "./graphs-view";

/** The selectable ranges in minutes, narrowest first. */
export const RANGE_MINUTES = [5, 15, 30, 60, 360, 1440] as const;

/** True when the logged history has at least two points, enough to draw a line. */
export function hasHistory(view: GraphsView | null): boolean {
  return view !== null && view.kind === "data" && view.timestamps.length >= 2;
}

/**
 * The next wider range than `minutes`, or null at the widest. Used to default
 * the page to a range that has data when the viewer has not chosen one: the
 * statistics log is sampled every few minutes, so "last 5 minutes" is
 * usually empty.
 */
export function widerRange(minutes: number): number | null {
  const next = RANGE_MINUTES.find((m) => m > minutes);
  return next === undefined ? null : next;
}

/** "History every 10 min" from the log's own cadence; null when it has under two points. */
export function historyCadenceText(view: GraphsView | null): string | null {
  if (view === null || view.kind !== "data") return null;
  const every = intervalLabel(view.timestamps);
  return every === null ? null : `History every ${every}`;
}

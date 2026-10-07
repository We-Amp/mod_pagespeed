// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** "Since when" labels for headline numbers; see sinceLabel. */

import { formatIsoTitle } from "./format";

/** Windows younger than this are "warming up": percentages are withheld. */
export const WARMUP_MS = 15 * 60_000;

/** How far ahead of the sample a start may sit and still be believed. */
const FUTURE_TOLERANCE_MS = 5 * 60_000;

export interface SinceLabel {
  text: string;
  /** ISO timestamp for a tooltip, or null when the base is unknown. */
  iso: string | null;
  warming: boolean;
}

/** A relative "since …" label for a start time: "since 5 min", "since 3 h",
 * "since 2 d" — or "since restart" when the start is unknown (absent,
 * non-finite, zero or below) or in the future by more than five minutes
 * (a start slightly ahead is clock skew between hosts and still labels;
 * further ahead is not a believable start). */
export function sinceLabel(
  startMs: number | null | undefined,
  nowMs: number,
): SinceLabel {
  const start =
    typeof startMs === "number" && Number.isFinite(startMs) && startMs > 0 ? startMs : null;
  if (start === null || start > nowMs + FUTURE_TOLERANCE_MS) {
    return { text: "since restart", iso: null, warming: false };
  }
  const ageMs = Math.max(0, nowMs - start);
  let text: string;
  if (ageMs < 60_000) text = "since 1 min";
  else if (ageMs < 60 * 60_000) text = `since ${Math.floor(ageMs / 60_000)} min`;
  else if (ageMs < 24 * 60 * 60_000) text = `since ${Math.floor(ageMs / 3_600_000)} h`;
  else text = `since ${Math.floor(ageMs / 86_400_000)} d`;
  return { text, iso: formatIsoTitle(start), warming: ageMs < WARMUP_MS };
}

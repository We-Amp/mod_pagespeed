// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { formatRelative } from "./format";

/** A stamp older than this many refresh intervals, with auto-refresh on, is overdue. */
export const OVERDUE_INTERVALS = 3;

/**
 * The "updated …" stamp next to a page's refresh controls. `intervalMs` is
 * the page's refresh interval: with auto-refresh on, a stamp that has missed
 * several refreshes (the server is not answering, or is busy) says so.
 */
export function updatedText(
  updatedAt: number | null,
  now: number,
  autoRefresh: boolean,
  intervalMs?: number,
): string {
  if (updatedAt === null) return "updated —";
  const rel = formatRelative(updatedAt, now);
  if (!autoRefresh) return `paused · updated ${rel}`;
  const overdue = intervalMs !== undefined && intervalMs > 0 && now - updatedAt > OVERDUE_INTERVALS * intervalMs;
  return overdue ? `refresh overdue · updated ${rel}` : `updated ${rel}`;
}

/** How long the result of a refresh the viewer asked for stays next to the stamp. */
export const REFRESH_RESULT_MS = 5000;

/**
 * The words for how a refresh the viewer asked for ended, from the poller's
 * outcome (lib/api/poller.ts); anything else, a page's own refresh that
 * returns nothing included, is an answer.
 */
export function refreshResultText(outcome: unknown): string {
  if (outcome === "failed") return "Refresh failed";
  if (outcome === "busy") return "Server busy; not refreshed";
  return "Refreshed";
}

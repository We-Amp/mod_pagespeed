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

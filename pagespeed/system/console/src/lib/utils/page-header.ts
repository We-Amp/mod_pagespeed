// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { isUnavailable } from "$lib/api/client";
import { isBusyAnswer, type RefreshOutcome } from "$lib/api/poller";
import { errorCode, isDaemonUnavailable } from "./daemon";
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
 * The words for how a refresh the viewer asked for ended, from the outcome
 * a page reports (lib/api/poller.ts, pageOutcome below). Only an answer is
 * "Refreshed"; a leaf the server turned off answered too.
 */
export function refreshResultText(outcome: RefreshOutcome): string {
  switch (outcome) {
    case "ok":
    case "off":
      return "Refreshed";
    case "partial":
      return "Partly refreshed; see the notice";
    case "busy":
      return "Server busy; not refreshed";
    default:
      return "Refresh failed";
  }
}

/**
 * A part of a page's read that failed. A state the server reports on
 * purpose is an answer: a view turned off, a whole-server-only view on a
 * per-host console, no optimizer configured or a version without the
 * endpoint (lib/utils/daemon.ts), another tab's busy read.
 */
export function failedRead(error: unknown): boolean {
  return (
    error instanceof Error &&
    !isUnavailable(error) &&
    // Not configured or not supported is a state; not reachable (502) is a failure.
    !(isDaemonUnavailable(error) && (error as { status?: number }).status !== 502) &&
    !isBusyAnswer(error) &&
    errorCode(error) !== "whole_server_console_only"
  );
}

/**
 * The outcome of a page that reads several parts and shows each part's
 * failure in place: the poller's "ok" only says the read settled. `parts`
 * are the page's own error states after the refresh, an error or `true`
 * for a part that failed: some failed is "partial", all failed is "failed".
 */
export function pageOutcome(outcome: RefreshOutcome, parts: readonly unknown[]): RefreshOutcome {
  if (outcome !== "ok") return outcome;
  const failed = parts.filter((p) => p === true || failedRead(p)).length;
  return failed === 0 ? "ok" : failed === parts.length ? "failed" : "partial";
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Presentation helpers for the daemon panels: formatting, and normalization
 * of the daemon's management-API JSON.
 *
 * The daemon is an untrusted peer whose schema evolves independently of the
 * console (older daemons omit newer fields), so everything here accepts
 * partial/unknown shapes and degrades rather than throws. Kept out of the
 * components so the behaviour is unit-testable.
 */

import { ApiError } from "$lib/api/client";
import type {
  DaemonCooldownEntry,
  DaemonCooldownsResponse,
} from "$lib/api/types";

/**
 * Whether an error from a /v1/daemon/* endpoint means "no daemon here":
 * 404 — this module build does not serve the daemon proxy endpoints;
 * 501 — this daemon build does not serve the specific endpoint;
 * 502 — the module could not reach the daemon;
 * 503 — no daemon transport is configured on this port/context.
 * All are normal operating states; the panels render an empty state, never
 * a raw error, with `daemonUnavailableReason()` naming which one.
 */
export function isDaemonUnavailable(error: Error | null): boolean {
  return (
    error instanceof ApiError &&
    (error.status === 404 ||
      error.status === 501 ||
      error.status === 502 ||
      error.status === 503)
  );
}

/**
 * The backend's `error` code out of an ApiError's detail, whether that
 * detail is already the extracted string (as AdminApiClient's
 * errorFromResponse produces it in production) or the raw JSON body (as
 * tests may pass directly) -- reads the `error` field when the detail
 * parses as JSON, else takes the detail as-is.
 */
export function errorCode(error: Error | null): string {
  if (!(error instanceof ApiError)) return "";
  const prefix = `HTTP ${error.status}: `;
  const detail = error.message.startsWith(prefix)
    ? error.message.slice(prefix.length)
    : error.message;
  try {
    const parsed = JSON.parse(detail) as { error?: unknown };
    if (typeof parsed.error === "string") return parsed.error;
  } catch {
    // Not JSON: `detail` is already the extracted reason string.
  }
  return detail;
}

/**
 * The module's own 403 body for a leaf gated to the whole-server console,
 * synthesized locally with no network request involved. `errorCode()` reads
 * it exactly like the real answer -- for a page that already knows (via
 * `knownPerVhostScope`) that asking would only reproduce it, so the page can
 * render the same explanation without ever sending the request.
 */
export function wholeServerConsoleOnlyError(): ApiError {
  return new ApiError(403, "whole_server_console_only");
}

/**
 * Operator-facing sentence for why a daemon panel has nothing to show,
 * shown under the "Daemon Unreachable" heading. Maps the daemon proxy's
 * three reason codes; anything else (including a genuine transport
 * failure/timeout) defaults to "unreachable".
 */
export function daemonUnavailableReason(error: Error | null): string {
  switch (errorCode(error)) {
    case "daemon_not_configured":
      return "not configured on this server";
    case "endpoint_unsupported_by_daemon":
      return "this optimizer version does not provide this panel";
    case "daemon_unreachable":
    default:
      return "unreachable";
  }
}

/** Object.entries for a plain object; anything else degrades to no entries. */
export function objectEntries(value: unknown): Array<[string, unknown]> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return [];
  }
  return Object.entries(value);
}

/**
 * Render an untrusted JSON field as short display text. Numbers get locale
 * grouping; objects/arrays get compact JSON (Svelte escapes the result, so a
 * hostile payload renders as inert text).
 */
export function fieldValue(value: unknown): string {
  if (value === undefined || value === null) return "\u2014";
  if (typeof value === "number") {
    return Number.isFinite(value) ? value.toLocaleString() : "\u2014";
  }
  if (typeof value === "boolean") return value ? "yes" : "no";
  if (typeof value === "string") return value === "" ? "\u2014" : value;
  return JSON.stringify(value) ?? "\u2014";
}

/**
 * Normalize the cooldowns response into a flat list. The daemon may return a
 * bare array or an object wrapping one; anything else (or absent) is an empty
 * list, which the panel renders as "no URLs in cooldown" — never an error.
 */
export function normalizeCooldowns(
  raw: DaemonCooldownsResponse | null | undefined,
): DaemonCooldownEntry[] {
  if (raw === null || raw === undefined) return [];
  const list = Array.isArray(raw) ? raw : raw.cooldowns;
  if (!Array.isArray(list)) return [];
  return list.filter(
    (entry): entry is DaemonCooldownEntry =>
      entry !== null && typeof entry === "object",
  );
}

const COOLDOWN_REASON_LABELS: Record<string, string> = {
  processing: "Processing",
  write_failure: "Write failed",
  revalidation: "Revalidating",
};

/**
 * A cooldown reason as a short operator-facing label, or null when the daemon
 * sent nothing usable: an absent, empty or unrecognised reason is just
 * "in cooldown" at the call site. The raw codes are daemon vocabulary and
 * never render (a non-string would print as "[object Object]").
 */
export function cooldownReasonLabel(reason: unknown): string | null {
  if (typeof reason !== "string" || reason.length === 0) return null;
  return COOLDOWN_REASON_LABELS[reason] ?? null;
}

/**
 * Flatten the numeric leaves of a serve-savings block into name/value rows
 * for a table, dot-joining nested keys ("images.rewrites"). Non-numeric
 * leaves are skipped — the table is a counter view, not a JSON dump.
 */
export function counterRows(
  block: Record<string, unknown> | undefined,
  prefix = "",
): Array<{ name: string; value: number }> {
  if (!block) return [];
  const rows: Array<{ name: string; value: number }> = [];
  for (const [key, value] of Object.entries(block)) {
    const name = prefix === "" ? key : `${prefix}.${key}`;
    if (typeof value === "number" && Number.isFinite(value)) {
      rows.push({ name, value });
    } else if (value !== null && typeof value === "object" && !Array.isArray(value)) {
      rows.push(...counterRows(value as Record<string, unknown>, name));
    }
  }
  return rows;
}

/** One health check's result as a sysadmin reads it. */
export interface CheckResult {
  text: string;
  /** true: passing; false: failing; null: the optimizer reported something else. */
  pass: boolean | null;
}

export function checkResult(value: unknown): CheckResult {
  if (typeof value === "boolean") return { text: value ? "Pass" : "Fail", pass: value };
  if (value !== null && typeof value === "object" && !Array.isArray(value)) {
    const v = value as Record<string, unknown>;
    if (typeof v.pass === "boolean") {
      const detail = [v.detail, v.message, v.reason, v.error].find(
        (d): d is string => typeof d === "string" && d !== "",
      );
      if (v.pass) return { text: "Pass", pass: true };
      return { text: detail ? `Fail: ${detail}` : "Fail", pass: false };
    }
  }
  return { text: fieldValue(value), pass: null };
}

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
 * 502 — the module could not reach the daemon (down or not configured);
 * 404 — this module build does not serve the daemon proxy endpoints.
 * Both are normal operating states; the panels render an empty state, never
 * a raw error.
 */
export function isDaemonUnavailable(error: Error | null): boolean {
  return (
    error instanceof ApiError &&
    (error.status === 502 || error.status === 404)
  );
}

/** Object.entries for a plain object; anything else degrades to no entries. */
export function objectEntries(value: unknown): Array<[string, unknown]> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return [];
  }
  return Object.entries(value);
}

/** "2d 5h 3m" / "41m 9s" / "12s"; "—" when absent or not a finite number. */
export function formatUptime(seconds: number | undefined): string {
  if (seconds === undefined || !Number.isFinite(seconds) || seconds < 0) {
    return "\u2014";
  }
  const s = Math.floor(seconds);
  const days = Math.floor(s / 86400);
  const hours = Math.floor((s % 86400) / 3600);
  const minutes = Math.floor((s % 3600) / 60);
  const secs = s % 60;
  if (days > 0) return `${days}d ${hours}h ${minutes}m`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m ${secs}s`;
  return `${secs}s`;
}

/** "12.3 MB" style; "—" when absent or not a finite number. */
export function formatBytes(bytes: number | undefined): string {
  if (bytes === undefined || !Number.isFinite(bytes) || bytes < 0) {
    return "\u2014";
  }
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  const rounded = unit === 0 ? String(value) : value.toFixed(1);
  return `${rounded} ${units[unit]}`;
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

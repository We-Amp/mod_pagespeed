// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Why the module left content as it was: one entry per module counter whose
 * definition names a reason, read from the counter as the module defines it
 * (each one is incremented exactly where the module gives up on a resource).
 * NOT_OPTIMIZED has a fix; ALREADY_GOOD needs none (the result was no
 * smaller). The Savings page lists both. The counts only grow and are mostly
 * context (a third-party host is a domain rejection on every page view), so
 * only DUE_TO_LOAD is also a finding. Nothing is said while the web server
 * warms up, and a counter the module does not report is left out, never 0.
 */

import { WARMUP_MS } from "./since";

export interface Reason {
  counter: string;
  label: string;
  fix?: string;
  /** The setting the fix names and its built-in default, shown with its current value. */
  option?: [name: string, fallback: string];
}

/** The one reason that is also a finding (alerts.ts): it is worth acting on while it rises. */
export const DUE_TO_LOAD = "image_rewrites_dropped_due_to_load";

export const NOT_OPTIMIZED: readonly Reason[] = [
  {
    counter: "resource_url_domain_rejections",
    label: "Domain not authorized",
    fix: "Authorize the domains you serve resources from, such as a CDN, with the Domain setting; third-party hosts can stay as they are.",
  },
  {
    counter: "num_cache_control_not_rewritable_resources",
    label: "Not safe to optimize",
    fix: "Send these resources with caching headers that allow storing them, without no-transform. Failed fetches count here too; see Resource fetch failures.",
  },
  {
    counter: "css_filter_parse_failures",
    label: "CSS the parser could not read",
    fix: "Logs names them. They are served as they are, with only their URLs rewritten when Fallback Rewrite Css is on.",
  },
  {
    counter: "javascript_minification_failures",
    label: "Scripts not minified",
    fix: "The minifier could not process these; Logs names them. They are served as they are.",
  },
  {
    counter: DUE_TO_LOAD,
    label: "Image work dropped under load",
    fix: "Raise ImageMaxRewritesAtOnce if the server has spare CPU, or add capacity.",
    option: ["ImageMaxRewritesAtOnce", "8"],
  },
  {
    counter: "image_rewrites_dropped_mime_type_unknown",
    label: "Image format unknown",
    fix: "Check that those URLs serve real, intact images; Logs names them.",
  },
];

export const ALREADY_GOOD: readonly Reason[] = [
  { counter: "image_rewrites_dropped_nosaving_noresize", label: "Recompressed image no smaller" },
  { counter: "image_rewrites_dropped_nosaving_resize", label: "Resized image no smaller" },
];

/** A counter's value; undefined when absent or not a number. */
export function reasonCount(vars: Record<string, unknown> | null, counter: string): number | undefined {
  const v = vars?.[counter];
  return typeof v === "number" && Number.isFinite(v) ? v : undefined;
}

/**
 * True when the module counters are past warm-up at `atMs`: a module start
 * that is unknown, or less than WARMUP_MS before, says nothing yet.
 */
export function pastWarmup(vars: Record<string, unknown> | null, atMs: number | undefined): boolean {
  const start = reasonCount(vars, "process_start_ms");
  return start !== undefined && start > 0 && atMs !== undefined && atMs - start >= WARMUP_MS;
}

/** The reasons with a count above 0, in table order; null while warming up. */
export function reasonRows(
  vars: Record<string, unknown> | null,
  atMs: number | undefined,
  reasons: readonly Reason[],
): { reason: Reason; count: number }[] | null {
  if (!pastWarmup(vars, atMs)) return null;
  return reasons.flatMap((reason) => {
    const count = reasonCount(vars, reason.counter) ?? 0;
    return count > 0 ? [{ reason, count }] : [];
  });
}

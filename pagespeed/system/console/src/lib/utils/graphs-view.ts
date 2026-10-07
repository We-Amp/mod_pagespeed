// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * What a graphs reply means. The module answers "{}" while its statistics
 * log has no samples at all, "{timestamps: [], …}" when the log has none in
 * the requested range, and 404/501 or an `error` body where there is no
 * graphs endpoint. The reply is read defensively: a malformed timestamp is
 * dropped, and a malformed sample becomes a gap (`null`) rather than being
 * dropped, so a series stays aligned with `timestamps` index-for-index.
 */

import { ApiError } from "$lib/api/client";
import type { Sample } from "./graph-series";

export interface GraphSeries {
  name: string;
  data: Sample[];
}

export type GraphsView =
  | { kind: "absent"; detail: string }
  | { kind: "no-log" }
  | { kind: "empty-range" }
  | { kind: "data"; timestamps: number[]; graphs: GraphSeries[] };

const numbers = (v: unknown): number[] =>
  Array.isArray(v) ? v.filter((x): x is number => typeof x === "number" && Number.isFinite(x)) : [];

/**
 * Like `numbers`, but for a graph's own samples: a non-numeric entry becomes
 * a gap (`null`) rather than being dropped, so the series stays aligned with
 * `timestamps` index-for-index. Dropping it instead would slide every later
 * sample one step earlier relative to its own timestamp.
 */
const samples = (v: unknown): Sample[] =>
  Array.isArray(v) ? v.map((x) => (typeof x === "number" && Number.isFinite(x) ? x : null)) : [];

export function graphsView(body: unknown): GraphsView {
  if (body === null || typeof body !== "object" || Array.isArray(body)) {
    return { kind: "absent", detail: "unexpected reply" };
  }
  const b = body as Record<string, unknown>;
  if (typeof b.error === "string" && b.error !== "") return { kind: "absent", detail: b.error };
  if (!("timestamps" in b) && !("variables" in b) && !("graphs" in b)) return { kind: "no-log" };
  const timestamps = numbers(b.timestamps);
  let graphs: GraphSeries[] = [];
  if (Array.isArray(b.graphs)) {
    graphs = b.graphs
      .filter(
        (g): g is { name: string; data?: unknown } =>
          g !== null && typeof g === "object" && typeof (g as { name?: unknown }).name === "string",
      )
      .map((g) => ({ name: g.name, data: samples(g.data) }));
  } else if (b.variables !== null && typeof b.variables === "object" && !Array.isArray(b.variables)) {
    graphs = Object.entries(b.variables as Record<string, unknown>).map(([name, values]) => ({
      name,
      data: samples(values),
    }));
  }
  if (timestamps.length === 0 || graphs.every((g) => g.data.length === 0)) return { kind: "empty-range" };
  return { kind: "data", timestamps, graphs };
}

/** The server has no graphs endpoint here (this build, or this configuration). */
export function isGraphsAbsent(error: Error): boolean {
  return error instanceof ApiError && (error.status === 404 || error.status === 501);
}

/** The typical time between samples, for "per <interval>" labels. */
export function intervalLabel(timestamps: readonly number[]): string | null {
  const gaps: number[] = [];
  for (let i = 1; i < timestamps.length; i++) {
    const gap = timestamps[i] - timestamps[i - 1];
    if (gap > 0) gaps.push(gap);
  }
  if (gaps.length === 0) return null;
  gaps.sort((a, b) => a - b);
  const median = gaps[Math.floor(gaps.length / 2)];
  if (median < 60_000) return `${Math.max(1, Math.round(median / 1000))} s`;
  if (median < 3_600_000) return `${Math.round(median / 60_000)} min`;
  return `${Math.round(median / 3_600_000)} h`;
}

/** This host's per-vhost console, next to the built-in global admin path; null when renamed. */
export function perVhostConsoleHref(basePath: string): string | null {
  return /\/pagespeed_global_admin$/.test(basePath)
    ? `${basePath.replace(/pagespeed_global_admin$/, "pagespeed_admin")}/#/graphs`
    : null;
}

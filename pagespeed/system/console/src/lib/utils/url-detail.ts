// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The URL detail view's data layer: normalize the daemon's alternates into
 * display rows (every field guarded — the daemon is untrusted), decide
 * which variants can be previewed, sort them, and keep the two epoch units
 * straight (last_access is milliseconds, cache_inserted_at is seconds).
 */

import type { DaemonAlternate, DaemonAlternatesResponse } from "$lib/api/types";
import { formatToMimeType } from "./quality";
import type { UrlStatus } from "./url-status";

export interface VariantRow {
  id: number | null;
  /** Unique within one built list (position + id): duplicate ids key safe. */
  rowKey: string;
  isSentinel: boolean;
  sentinelName: string | null;
  format: string;
  viewport: string;
  density: string;
  saveData: boolean;
  encoding: string;
  typeClass: string;
  mimeType: string;
  contentClass: string;
  size: number | null;
  originalSize: number | null;
  quality: number | null;
  hits: number;
  lastAccessMs: number | null;
  cachedAtMs: number | null;
  raw: DaemonAlternate;
}

const numOrNull = (v: unknown): number | null =>
  typeof v === "number" && Number.isFinite(v) ? v : null;
const nonNegInt = (v: unknown): number | null => {
  const n = numOrNull(v);
  return n !== null && Number.isInteger(n) && n >= 0 ? n : null;
};
const str = (v: unknown): string => (typeof v === "string" ? v : "");

/** The widest epoch-ms value `Date` can hold: past it, `toISOString()` throws. */
const MAX_EPOCH_MS = 8.64e15;

/** last_access is epoch MILLISECONDS; null unless finite, > 0 and inside the Date range. */
export function epochMsOf(value: unknown): number | null {
  const n = numOrNull(value);
  return n !== null && n > 0 && n <= MAX_EPOCH_MS ? n : null;
}

/** cache_inserted_at is epoch SECONDS; returned as MILLISECONDS, bounded like epochMsOf. */
export function epochSecToMs(value: unknown): number | null {
  const n = numOrNull(value);
  return n !== null && n > 0 && n * 1000 <= MAX_EPOCH_MS ? n * 1000 : null;
}

export function toVariantRows(
  body: DaemonAlternatesResponse | null | undefined,
): VariantRow[] {
  const list = body?.alternates;
  if (!Array.isArray(list)) return [];
  const rows: VariantRow[] = [];
  for (const entry of list) {
    if (entry === null || typeof entry !== "object") continue;
    const format = str(entry.format);
    const id = nonNegInt(entry.alternate_id);
    rows.push({
      id,
      rowKey: `${rows.length}:${id ?? "no-id"}`,
      isSentinel: entry.is_sentinel === true,
      sentinelName: typeof entry.sentinel_name === "string" ? entry.sentinel_name : null,
      format,
      viewport: str(entry.viewport),
      density: str(entry.density),
      saveData: entry.save_data === true,
      encoding: str(entry.encoding),
      typeClass: str(entry.content_type),
      mimeType: formatToMimeType(format, str(entry.origin_content_type) || undefined),
      contentClass: str(entry.content_class),
      size: nonNegInt(entry.size),
      originalSize: nonNegInt(entry.original_size),
      // NaN is a number for typeof — numOrNull rejects it via isFinite.
      quality: numOrNull(entry.ssimulacra2_score),
      hits: nonNegInt(entry.hit_count) ?? 0,
      lastAccessMs: epochMsOf(entry.last_access),
      cachedAtMs: epochSecToMs(entry.cache_inserted_at),
      raw: entry,
    });
  }
  return rows;
}

/** The module's content gate: exactly these five media types are served. */
export const PREVIEW_MIME_TYPES: ReadonlySet<string> = new Set([
  "image/png",
  "image/jpeg",
  "image/gif",
  "image/webp",
  "image/avif",
]);

/**
 * Whether a variant gets an image preview: an image-class variant whose
 * resolved MIME the module's gate serves (SVG and text variants would
 * answer 415, so no <img> is created for them).
 */
export function previewable(row: VariantRow): boolean {
  return (
    !row.isSentinel &&
    row.id !== null &&
    row.typeClass === "image" &&
    PREVIEW_MIME_TYPES.has(row.mimeType)
  );
}

export type VariantSortKey = "format" | "size" | "quality" | "hits" | "lastAccess";
export interface VariantSortState {
  key: VariantSortKey;
  asc: boolean;
}
/** The detail table opens with the most-served variant first. */
export const DEFAULT_VARIANT_SORT: VariantSortState = { key: "hits", asc: false };

export function nextVariantSortState(
  current: VariantSortState,
  key: VariantSortKey,
): VariantSortState {
  if (current.key === key) return { key, asc: !current.asc };
  return { key, asc: key === "format" };
}

export function sortVariantRows(rows: VariantRow[], sort: VariantSortState): VariantRow[] {
  const value = (row: VariantRow): number | null =>
    sort.key === "size"
      ? row.size
      : sort.key === "quality"
        ? row.quality
        : sort.key === "hits"
          ? row.hits
          : sort.key === "lastAccess"
            ? row.lastAccessMs
            : null;
  const sorted = [...rows];
  sorted.sort((a, b) => {
    if (sort.key === "format") {
      const cmp = (a.format || a.sentinelName || "").localeCompare(
        b.format || b.sentinelName || "",
      );
      return sort.asc ? cmp : -cmp;
    }
    const av = value(a);
    const bv = value(b);
    // Absent values sort last in both directions.
    if (av === null && bv === null) return 0;
    if (av === null) return 1;
    if (bv === null) return -1;
    const cmp = av - bv;
    if (cmp === 0) return (a.id ?? 0) - (b.id ?? 0);
    return sort.asc ? cmp : -cmp;
  });
  return sorted;
}

const SENTINEL_LABELS: Record<string, string> = {
  original_content: "Original content record",
  early_hints: "Early hints record",
  warmup: "Warmup request record",
  content_hash: "Content hash record",
  subresource_manifest: "Subresource manifest record",
  browser_profile: "Browser profile record",
  headers_sidecar: "Response headers record",
  agent_markdown: "Agent markdown record",
  llms_txt: "llms.txt record",
  llms_txt_meta: "llms.txt metadata record",
  negative_verdict: "Negative verdict record",
  decline_tombstone: "Decline tombstone record",
};

/**
 * What a sentinel row says instead of the daemon's raw sentinel name: a
 * plain-language label for the optimizer's bookkeeping entries. An unknown
 * name is an internal record; an absent one reads as a pending entry.
 */
export function sentinelLabel(name: string | null): string {
  if (name === null || name.length === 0) return "Pending";
  return SENTINEL_LABELS[name] ?? "Internal record";
}

export function statusLabel(status: UrlStatus | null): string {
  switch (status) {
    case "complete":
      return "Complete";
    case "partial":
      return "Partial";
    case "original-only":
      return "Original only";
    case "revalidating":
      return "Revalidating";
    case null:
      return "Nothing cached";
  }
}

/**
 * The id of the optimizer's "original_content" record: the cached original
 * bytes, stored as a sentinel without a viewport, density or format of its
 * own. A variant is compared against it when no same-shaped original entry
 * exists. Null when there is none, it is empty, or its id is unusable.
 */
export function originalContentId(rows: readonly VariantRow[]): number | null {
  for (const row of rows) {
    if (!row.isSentinel || row.sentinelName !== "original_content") continue;
    if (row.id === null || !Number.isInteger(row.id) || row.id < 0) continue;
    if (row.size === null || row.size <= 0) continue;
    return row.id;
  }
  return null;
}

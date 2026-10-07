// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type {
  CacheEntryKey,
  DaemonCacheUrlsResponse,
  DaemonCooldownEntry,
} from "$lib/api/types";
import { normalizeLensHost } from "./host-lens";

export type UrlSortKey = "url" | "host" | "variants";
export interface UrlSortState {
  key: UrlSortKey;
  asc: boolean;
}
export const DEFAULT_URL_SORT: UrlSortState = { key: "url", asc: true };

export interface UrlRow {
  key: CacheEntryKey | null; // null: the entry cannot be opened (bad host/scheme/path)
  /** Unique within one built page (position + display): duplicate entries key safe. */
  rowKey: string;
  display: string; // "https://www.example.test/hero.png"
  host: string;
  variants: number;
  cooldown: DaemonCooldownEntry | null;
}

export interface PageWindow {
  from: number; // 1-based first row of the page; 0 when empty
  to: number; // 1-based last row shown
  total: number | null; // null when the daemon's total is missing/garbage
  hasMore: boolean;
}

/** One page of the index is 50 rows; the module caps `limit` at 500. */
export const URLS_PAGE_SIZE = 50;

/**
 * The module's `url` rule: the origin-form path + query the module
 * records — a leading "/", at most 2048 bytes, printable ASCII (0x21–0x7E)
 * only, no "#". A value that fails it would be a 400 from the module.
 */
export function isValidEntryPath(url: string): boolean {
  if (url.length === 0 || url.length > 2048 || url[0] !== "/") return false;
  for (let i = 0; i < url.length; i++) {
    const c = url.charCodeAt(i);
    if (c < 0x21 || c > 0x7e || c === 0x23 /* # */) return false;
  }
  return true;
}

const HOST_RE = /^[A-Za-z0-9._:[\]-]{1,253}$/;

/** A CacheEntryKey from untrusted parts, or null when any part is unusable. */
export function entryKey(url: unknown, host: unknown, scheme: unknown): CacheEntryKey | null {
  if (typeof url !== "string" || !isValidEntryPath(url)) return null;
  if (typeof host !== "string" || !HOST_RE.test(host)) return null;
  if (scheme !== "http" && scheme !== "https") return null;
  return { url, host, scheme };
}

/** "https://www.example.test/hero.png" — for headings, filters and alt text. */
export function displayUrl(key: CacheEntryKey): string {
  return `${key.scheme}://${key.host}${key.url}`;
}

/** The detail route for an entry: path + query, host and scheme as three hash parameters. */
export function detailHref(key: CacheEntryKey): string {
  const params = new URLSearchParams({ url: key.url, host: key.host, scheme: key.scheme });
  return `#/urls/detail?${params.toString()}`;
}

/** The detail route's entry, or null when any of the three is missing or invalid. */
export function parseDetailKey(params: URLSearchParams): CacheEntryKey | null {
  return entryKey(params.get("url"), params.get("host"), params.get("scheme"));
}

/**
 * Splits a full URL (the Cache lookup's, an operator's paste) into the
 * entry the optimizer would hold for it, with the URL API — which
 * canonicalizes as the module does: lowercase host without port,
 * percent-encoded path, "/" for an empty path.
 */
export function entryKeyFromAbsoluteUrl(absolute: string): CacheEntryKey | null {
  let parsed: URL;
  try {
    parsed = new URL(absolute);
  } catch {
    return null;
  }
  const scheme = parsed.protocol.replace(/:$/, "");
  return entryKey(`${parsed.pathname || "/"}${parsed.search}`, parsed.hostname, scheme);
}

/** A page of the whole-server console next to the built-in per-vhost admin
 * path (a prefixed mount included); null when the admin path is renamed
 * (then no link is shown). `hash` is the route, e.g. "#/urls". */
export function globalConsoleHref(basePath: string, hash: string): string | null {
  return /\/pagespeed_admin$/.test(basePath)
    ? `${basePath.replace(/pagespeed_admin$/, "pagespeed_global_admin")}/${hash}`
    : null;
}

/** The whole-server console's URLs page next to the built-in per-vhost
 * admin path; null when the admin path is renamed (then no link is shown). */
export function globalConsoleUrlsHref(basePath: string): string | null {
  return globalConsoleHref(basePath, "#/urls");
}

const asCount = (value: unknown): number =>
  typeof value === "number" && Number.isInteger(value) && value >= 0 ? value : 0;

const cooldownKey = (url: unknown, host: unknown, scheme: unknown): string =>
  `${typeof scheme === "string" ? scheme : ""}://${typeof host === "string" ? host : ""}${
    typeof url === "string" ? url : ""
  }`;

/**
 * The rows of one page of the index, joined with the cooldown list on the
 * full entry key (scheme + host + path), then filtered and sorted
 * CLIENT-SIDE on this page only (server-side filter/sort does not exist
 * upstream; the page says so). `lensHost`: only that host's rows, compared
 * exactly after the lens's own normalization -- the index is asked for that
 * host alone, and this keeps a previous host's page or an answer that
 * ignored the request from showing other hosts.
 */
export function buildUrlRows(
  body: DaemonCacheUrlsResponse | undefined,
  cooldowns: DaemonCooldownEntry[],
  search: string,
  sort: UrlSortState,
  lensHost: string | null = null,
): UrlRow[] {
  const entries = Array.isArray(body?.urls) ? body.urls : [];
  const cooldownByKey = new Map<string, DaemonCooldownEntry>();
  for (const entry of cooldowns) {
    if (entry && typeof entry.url === "string") {
      cooldownByKey.set(cooldownKey(entry.url, entry.hostname, entry.scheme), entry);
    }
  }
  let rows: UrlRow[] = [];
  for (const entry of entries) {
    if (entry === null || typeof entry !== "object") continue;
    if (typeof entry.url !== "string" || entry.url.length === 0) continue;
    const host = typeof entry.hostname === "string" ? entry.hostname : "";
    if (lensHost !== null && normalizeLensHost(host) !== lensHost) continue;
    const scheme = entry.scheme === "http" || entry.scheme === "https" ? entry.scheme : "";
    const display = `${scheme}://${host}${entry.url}`;
    rows.push({
      key: entryKey(entry.url, host, scheme),
      rowKey: `${rows.length}:${display}`,
      display,
      host,
      variants: asCount(entry.alternate_count),
      cooldown: cooldownByKey.get(cooldownKey(entry.url, host, scheme)) ?? null,
    });
  }
  const query = search.trim().toLowerCase();
  if (query) {
    rows = rows.filter(
      (row) =>
        row.display.toLowerCase().includes(query) ||
        row.host.toLowerCase().includes(query),
    );
  }
  rows.sort((a, b) => {
    if (sort.key === "variants") {
      const cmp = a.variants - b.variants;
      // Equal counts keep a stable, readable url order in both directions.
      if (cmp === 0) return a.display.localeCompare(b.display);
      return sort.asc ? cmp : -cmp;
    }
    const av = sort.key === "host" ? a.host : a.display;
    const bv = sort.key === "host" ? b.host : b.display;
    const cmp = av.localeCompare(bv);
    return sort.asc ? cmp : -cmp;
  });
  return rows;
}

export function nextUrlSortState(current: UrlSortState, key: UrlSortKey): UrlSortState {
  if (current.key === key) return { key, asc: !current.asc };
  return { key, asc: key !== "variants" };
}

export function pageWindow(
  body: DaemonCacheUrlsResponse | undefined,
  requestedOffset: number,
  rowsOnPage: number,
): PageWindow {
  const total =
    typeof body?.total === "number" && Number.isInteger(body.total) && body.total >= 0
      ? body.total
      : null;
  return {
    from: rowsOnPage === 0 ? 0 : requestedOffset + 1,
    to: requestedOffset + rowsOnPage,
    total,
    hasMore: body?.has_more === true,
  };
}

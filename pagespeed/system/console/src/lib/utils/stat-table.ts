// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Presentation helpers for the Statistics table: sort state, filtering, and
 * the remembered "Description" column toggle.
 *
 * Kept out of the component so the behaviour is unit-testable.
 */

import { describeStat } from "$lib/data/stat-descriptions";

export type SortKey = "name" | "value" | "delta";

export interface SortState {
  key: SortKey;
  asc: boolean;
}

/** The table opens sorted by name, A->Z. */
export const DEFAULT_SORT: SortState = { key: "name", asc: true };

/**
 * The sort state after clicking a column header.
 *
 * Clicking the column that is already active flips the direction. Clicking a
 * new column uses that column's *natural* first direction: names read A->Z,
 * but values read biggest-first -- a server has hundreds of counters that are
 * legitimately zero, and sorting those to the top first would make the user
 * click twice to see anything.
 */
export function nextSortState(current: SortState, key: SortKey): SortState {
  if (current.key === key) {
    return { key, asc: !current.asc };
  }
  return { key, asc: key === "name" };
}

/** One row of the statistics table. */
export interface StatRow {
  name: string;
  value: number;
  description: string;
  delta: number | null;
}

/**
 * The families of counters, in the order the page shows them: what an
 * operator reads first comes first. Each family lists the name patterns it
 * owns, separated by spaces: a pattern matches the start of a name, or with a
 * leading "*" its end. The longest matching pattern of any family wins, so
 * "cache_flush_" (purges) beats "cache_" (the HTTP cache). The key is what
 * keeps a group's open or closed state across polls; never reuse one.
 */
export const STAT_FAMILIES: ReadonlyArray<readonly [key: string, label: string, patterns: string]> = [
  ["rewriting", "Page rewriting", "rewrite_ num_rewrites_ total_rewrite_ num_flushes num_deadline_ named-lock-rewrite- *-queue-depth current-expensive- csp_blocked_ domain_rewrites url_trim cache_extensions not_cacheable num_cache_control_ converted_meta_"],
  ["ipro", "In-place optimization", "ipro_ in_place_"],
  ["optimizer", "Optimizer serving", "ipro_daemon_ daemon_ zerocopy_"],
  ["html", "Optimizer HTML cache", "daemon_html_"],
  ["packs", "Rule packs", "rule_pack_"],
  ["http", "HTTP cache", "cache_ compressed_cache_ num_fallback_ num_conditional_ num_proactively_"],
  ["file", "File cache", "file_cache_ stdio_fs_"],
  ["shm", "Shared memory cache", "shm_cache_"],
  ["cyclone", "Cyclone cache", "cyclone_cache_"],
  ["external", "Memcached and Redis", "memcache redis_ cache_batcher_"],
  ["pcache", "Property cache", "pcache-"],
  ["fetch", "Origin fetches", "curl_ http_ dropped-fetch- queued-fetch- current-fetch- num_resource_fetch_ resource_url_ url_input_ font_service_ slurp_"],
  ["resources", "Optimized resource requests", "total_fetch_ resource_fetch resource_404"],
  ["images", "Images", "image_ num_dedup_inlined_images_ lazyload_ critical_images_ prioritize_critical_images"],
  ["css", "CSS", "css_ flatten_imports_ num_css_ critical_css_ critical_selectors_"],
  ["js", "JavaScript", "javascript_ js_ num_js_"],
  ["purge", "Purges and cache flushes", "purge_ _purge_ downstream_ successful_downstream_ cache_flush_"],
  ["beacons", "Beacons, ads and local storage", "page_load_ total_page_load_ beacon_ instrumentation_ show_ads_ num_local_storage_"],
  ["proxy", "Proxy-all mode", "proxy-all-mode-"],
  ["botauth", "Web bot authentication", "web_bot_auth_"],
  ["process", "Server process", "child_shutdown_ process_start_ timestamp_ statistics_"],
];

/** Where a counter no family claims goes: last, under an honest name. */
export const OTHER_FAMILY = { key: "other", label: "Other" } as const;

const PATTERNS: ReadonlyArray<readonly [pattern: string, family: number]> = STAT_FAMILIES.flatMap(
  ([, , patterns], i) => patterns.split(" ").map((p) => [p, i] as const),
);

/** The index into STAT_FAMILIES of a counter's family, or -1 for Other. */
function familyIndex(name: string): number {
  let best = -1;
  let bestLength = 0;
  for (const [pattern, i] of PATTERNS) {
    const hit = pattern[0] === "*" ? name.endsWith(pattern.slice(1)) : name.startsWith(pattern);
    if (hit && pattern.length > bestLength) {
      best = i;
      bestLength = pattern.length;
    }
  }
  return best;
}

/** The key of the family a counter belongs to ("other" when none claims it). */
export function statGroupKey(name: string): string {
  const i = familyIndex(name);
  return i < 0 ? OTHER_FAMILY.key : STAT_FAMILIES[i][0];
}

/** One collapsible family of counters. */
export interface StatGroup {
  key: string;
  label: string;
  rows: StatRow[];
}

/** Group rows by family, in the families' own order, Other last. Rows keep
 * the order they arrive in (the caller's sort). A family with no rows is
 * left out: the caller passes the rows it wants shown (already
 * search-filtered). */
export function groupStatRows(rows: StatRow[]): StatGroup[] {
  const buckets: StatRow[][] = STAT_FAMILIES.map(() => []);
  const other: StatRow[] = [];
  for (const row of rows) {
    const i = familyIndex(row.name);
    (i < 0 ? other : buckets[i]).push(row);
  }
  const groups: StatGroup[] = [];
  buckets.forEach((b, i) => {
    if (b.length) groups.push({ key: STAT_FAMILIES[i][0], label: STAT_FAMILIES[i][1], rows: b });
  });
  if (other.length) groups.push({ ...OTHER_FAMILY, rows: other });
  return groups;
}

/**
 * Build the rows to render: annotate each counter with its description, keep
 * the ones matching the search, and sort.
 *
 * The search matches the description as well as the name, so a user who does
 * not know the naming scheme can type "in-place" and find the `ipro_*` family.
 */
export function buildRows(
  variables: Record<string, number> | undefined,
  search: string,
  sort: SortState,
  deltas?: Map<string, number> | null,
): StatRow[] {
  if (!variables) return [];

  // The map's values ARE the deltas, precomputed by the caller
  // (current − baseline); a name absent from it gets null.
  let rows: StatRow[] = Object.entries(variables).map(([name, value]) => ({
    name,
    value,
    description: describeStat(name),
    delta: deltas?.get(name) ?? null,
  }));

  const query = search.trim().toLowerCase();
  if (query) {
    rows = rows.filter(
      (row) =>
        row.name.toLowerCase().includes(query) ||
        row.description.toLowerCase().includes(query),
    );
  }

  rows.sort((a, b) => {
    if (sort.key === "name") {
      const cmp = a.name.localeCompare(b.name);
      return sort.asc ? cmp : -cmp;
    }
    if (sort.key === "delta") {
      // Rows without a baseline sit at the bottom in either direction;
      // equal deltas (very common: all zero) keep the readable A->Z order.
      if (a.delta === null && b.delta === null) return a.name.localeCompare(b.name);
      if (a.delta === null) return 1;
      if (b.delta === null) return -1;
      const cmp = a.delta - b.delta;
      if (cmp === 0) return a.name.localeCompare(b.name);
      return sort.asc ? cmp : -cmp;
    }
    const cmp = a.value - b.value;
    // Equal values (very common -- most counters sit at zero) keep a stable,
    // readable A->Z order rather than whatever the dump happened to emit.
    if (cmp === 0) return a.name.localeCompare(b.name);
    return sort.asc ? cmp : -cmp;
  });

  return rows;
}

export const DESCRIPTION_COLUMN_STORAGE_KEY =
  "pagespeed.statistics.descriptionColumn";

/**
 * Whether the Description column is shown. Defaults to on -- the
 * descriptions are the page's only explanations; an explicit "off" is
 * remembered.
 */
export function loadDescriptionColumn(
  storage: Storage | null = defaultStorage(),
): boolean {
  try {
    return storage?.getItem(DESCRIPTION_COLUMN_STORAGE_KEY) !== "0";
  } catch {
    return true;
  }
}

/** Persist the toggle. A storage failure must never break the page. */
export function saveDescriptionColumn(
  enabled: boolean,
  storage: Storage | null = defaultStorage(),
): void {
  try {
    storage?.setItem(DESCRIPTION_COLUMN_STORAGE_KEY, enabled ? "1" : "0");
  } catch {
    // Ignore: the toggle still works for this page view, it just won't persist.
  }
}

/** `localStorage` when the environment has one; merely touching it can throw. */
function defaultStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

export const DELTA_COLUMN_STORAGE_KEY = "pagespeed.statistics.deltaColumn";

/**
 * Whether the "Δ since open" column is shown. Off by default: it answers a
 * question ("what moved?") rather than presenting the data itself.
 */
export function loadDeltaColumn(
  storage: Storage | null = defaultStorage(),
): boolean {
  try {
    return storage?.getItem(DELTA_COLUMN_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/** Persist the toggle. A storage failure must never break the page. */
export function saveDeltaColumn(
  enabled: boolean,
  storage: Storage | null = defaultStorage(),
): void {
  try {
    storage?.setItem(DELTA_COLUMN_STORAGE_KEY, enabled ? "1" : "0");
  } catch {
    // Ignore: the toggle still works for this page view, it just won't persist.
  }
}

/** A counter name split after each "_", "." or "-", so a narrow table can wrap it between words. */
export function nameSegments(name: string): string[] {
  return name.split(/(?<=[_.-])/);
}

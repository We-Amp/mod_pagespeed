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

/** The family a counter belongs to: the text before the first "_". */
export function statGroupKey(name: string): string {
  const cut = name.indexOf("_");
  return cut === -1 ? name : name.slice(0, cut);
}

/** Curated family labels for the prefixes a real server emits; anything
 * else falls through to the title-cased prefix (groupLabel). */
export const STAT_GROUP_LABELS: Readonly<Record<string, string>> = {
  ipro: "In-place optimization",
  cache: "Cache",
  image: "Images",
  javascript: "JavaScript",
  js: "JavaScript",
  css: "CSS",
  curl: "Origin fetches",
  memcache: "Memcached",
  memcached: "Memcached",
  redis: "Redis",
  purge: "Purges",
  http: "HTTP",
  num: "Counts",
  critical: "Critical selectors",
  lazyload: "Lazyload",
  shm: "Shared memory",
  file: "File cache",
  cyclone: "Cyclone cache",
  compressed: "Compressed cache",
  downstream: "Downstream cache",
  font: "Resource inputs",
  url: "Resource inputs",
};

function titleCase(key: string): string {
  return key === "" ? "" : key[0].toUpperCase() + key.slice(1);
}

function groupLabel(key: string): string {
  return STAT_GROUP_LABELS[key] ?? titleCase(key);
}

/** One collapsible family of counters. */
export interface StatGroup {
  key: string;
  label: string;
  rows: StatRow[];
}

/** Group rows by family: curated groups first (by label), then the rest
 * (by label). A group with no rows cannot occur — the caller passes the
 * rows it wants shown (already search-filtered). */
export function groupStatRows(rows: StatRow[]): StatGroup[] {
  const byKey = new Map<string, StatRow[]>();
  for (const row of rows) {
    const key = statGroupKey(row.name);
    byKey.set(key, [...(byKey.get(key) ?? []), row]);
  }
  const groups = [...byKey.entries()].map(([key, grouped]) => ({ key, label: groupLabel(key), rows: grouped }));
  return groups.sort((a, b) => {
    const aKnown = a.key in STAT_GROUP_LABELS ? 0 : 1;
    const bKnown = b.key in STAT_GROUP_LABELS ? 0 : 1;
    return aKnown - bKnown || a.label.localeCompare(b.label);
  });
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

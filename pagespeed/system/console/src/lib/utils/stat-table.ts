// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Presentation helpers for the Statistics table: sort state, filtering, and
 * the remembered "Description" column toggle.
 *
 * Kept out of the component so the behaviour is unit-testable.
 */

import { describeStat } from "$lib/data/stat-descriptions";

export type SortKey = "name" | "value";

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
): StatRow[] {
  if (!variables) return [];

  let rows: StatRow[] = Object.entries(variables).map(([name, value]) => ({
    name,
    value,
    description: describeStat(name),
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
 * Whether the Description column was left on. Defaults to off: the column is
 * wide, and the tooltip already covers the occasional lookup.
 */
export function loadDescriptionColumn(
  storage: Storage | null = defaultStorage(),
): boolean {
  try {
    return storage?.getItem(DESCRIPTION_COLUMN_STORAGE_KEY) === "1";
  } catch {
    return false;
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

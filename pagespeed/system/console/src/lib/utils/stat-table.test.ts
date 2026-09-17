// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import {
  DEFAULT_SORT,
  DESCRIPTION_COLUMN_STORAGE_KEY,
  buildRows,
  loadDescriptionColumn,
  nextSortState,
  saveDescriptionColumn,
} from "./stat-table";
import { NEUTRAL_STAT_DESCRIPTION } from "$lib/data/stat-descriptions";

/** Minimal in-memory Storage stand-in. */
function fakeStorage(initial: Record<string, string> = {}): Storage {
  const map = new Map(Object.entries(initial));
  return {
    get length() {
      return map.size;
    },
    clear: () => map.clear(),
    getItem: (k: string) => (map.has(k) ? map.get(k)! : null),
    key: (i: number) => [...map.keys()][i] ?? null,
    removeItem: (k: string) => void map.delete(k),
    setItem: (k: string, v: string) => void map.set(k, String(v)),
  } as Storage;
}

/** Storage that throws on every access, like Safari with cookies blocked. */
function throwingStorage(): Storage {
  const boom = () => {
    throw new Error("SecurityError: storage is not available");
  };
  return {
    get length(): number {
      return boom();
    },
    clear: boom,
    getItem: boom,
    key: boom,
    removeItem: boom,
    setItem: boom,
  } as unknown as Storage;
}

describe("nextSortState", () => {
  it("sorts values biggest-first on the first click", () => {
    // The point of the whole change: a server has hundreds of counters that
    // are legitimately zero, so ascending-first would show a screen of zeros.
    expect(nextSortState(DEFAULT_SORT, "value")).toEqual({
      key: "value",
      asc: false,
    });
  });

  it("sorts names A->Z on the first click", () => {
    expect(nextSortState({ key: "value", asc: false }, "name")).toEqual({
      key: "name",
      asc: true,
    });
  });

  it("flips the direction when the active column is clicked again", () => {
    const first = nextSortState(DEFAULT_SORT, "value");
    const second = nextSortState(first, "value");
    expect(second).toEqual({ key: "value", asc: true });
    expect(nextSortState(second, "value")).toEqual({ key: "value", asc: false });
  });

  it("returns to the natural direction when switching back to a column", () => {
    let sort = nextSortState(DEFAULT_SORT, "value"); // value, desc
    sort = nextSortState(sort, "value"); // value, asc
    sort = nextSortState(sort, "name"); // name, asc
    expect(nextSortState(sort, "value")).toEqual({ key: "value", asc: false });
  });

  it("opens the table sorted by name, ascending", () => {
    expect(DEFAULT_SORT).toEqual({ key: "name", asc: true });
  });
});

describe("buildRows", () => {
  // Laid out so a value sort disagrees with a name sort in BOTH directions:
  // by name it is hits/misses/ipro, by value descending misses/hits/ipro and
  // ascending ipro/hits/misses. Neither value order coincides with a name
  // order, so swapping the value comparator for a name comparator fails a
  // test rather than passing silently.
  const variables = {
    cache_hits: 5,
    cache_misses: 100,
    ipro_served: 0,
  };

  it("returns nothing when there is no data yet", () => {
    expect(buildRows(undefined, "", DEFAULT_SORT)).toEqual([]);
  });

  it("annotates every row with a description", () => {
    const rows = buildRows(variables, "", DEFAULT_SORT);
    expect(rows).toHaveLength(3);
    for (const row of rows) {
      expect(row.description.length).toBeGreaterThan(0);
    }
  });

  it("orders by value, biggest first, under a descending value sort", () => {
    const rows = buildRows(variables, "", { key: "value", asc: false });
    expect(rows.map((r) => r.name)).toEqual([
      "cache_misses",
      "cache_hits",
      "ipro_served",
    ]);
  });

  it("orders by value, smallest first, once the value sort is flipped", () => {
    const rows = buildRows(variables, "", { key: "value", asc: true });
    expect(rows.map((r) => r.name)).toEqual([
      "ipro_served",
      "cache_hits",
      "cache_misses",
    ]);
  });

  it("orders by name A->Z under the default sort", () => {
    const rows = buildRows(variables, "", DEFAULT_SORT);
    expect(rows.map((r) => r.name)).toEqual([
      "cache_hits",
      "cache_misses",
      "ipro_served",
    ]);
  });

  it("breaks value ties by name so equal counters stay readable", () => {
    const rows = buildRows({ zulu: 0, alpha: 0, mike: 0 }, "", {
      key: "value",
      asc: false,
    });
    expect(rows.map((r) => r.name)).toEqual(["alpha", "mike", "zulu"]);
  });

  it("filters on the name", () => {
    const rows = buildRows(variables, "ipro", DEFAULT_SORT);
    expect(rows.map((r) => r.name)).toEqual(["ipro_served"]);
  });

  it("filters on the description text as well as the name", () => {
    // "in-place" appears nowhere in the counter names, only in the prose.
    const rows = buildRows(variables, "in-place", DEFAULT_SORT);
    expect(rows.map((r) => r.name)).toEqual(["ipro_served"]);
  });

  it("matches descriptions case-insensitively and ignores stray spaces", () => {
    const rows = buildRows(variables, "  IN-PLACE  ", DEFAULT_SORT);
    expect(rows.map((r) => r.name)).toEqual(["ipro_served"]);
  });

  it("returns nothing when neither name nor description matches", () => {
    expect(
      buildRows(variables, "zzzznonexistent_xyzzy", DEFAULT_SORT),
    ).toEqual([]);
  });

  it("still describes a counter the table has never heard of", () => {
    const rows = buildRows({ totally_made_up_counter: 1 }, "", DEFAULT_SORT);
    expect(rows[0].description).toBe(NEUTRAL_STAT_DESCRIPTION);
  });
});

describe("the remembered Description column toggle", () => {
  it("defaults to off when nothing was stored", () => {
    expect(loadDescriptionColumn(fakeStorage())).toBe(false);
  });

  it("round-trips through storage", () => {
    const storage = fakeStorage();
    saveDescriptionColumn(true, storage);
    expect(storage.getItem(DESCRIPTION_COLUMN_STORAGE_KEY)).toBe("1");
    expect(loadDescriptionColumn(storage)).toBe(true);

    saveDescriptionColumn(false, storage);
    expect(loadDescriptionColumn(storage)).toBe(false);
  });

  it("ignores a stored value it does not recognise", () => {
    expect(
      loadDescriptionColumn(
        fakeStorage({ [DESCRIPTION_COLUMN_STORAGE_KEY]: "yes please" }),
      ),
    ).toBe(false);
  });

  it("falls back to off when there is no storage at all", () => {
    expect(loadDescriptionColumn(null)).toBe(false);
    expect(() => saveDescriptionColumn(true, null)).not.toThrow();
  });

  it("survives storage that throws on every access", () => {
    const storage = throwingStorage();
    expect(loadDescriptionColumn(storage)).toBe(false);
    expect(() => saveDescriptionColumn(true, storage)).not.toThrow();
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import {
  DEFAULT_SORT,
  DELTA_COLUMN_STORAGE_KEY,
  DESCRIPTION_COLUMN_STORAGE_KEY,
  STAT_GROUP_LABELS,
  buildRows,
  groupStatRows,
  loadDeltaColumn,
  loadDescriptionColumn,
  nameSegments,
  nextSortState,
  saveDeltaColumn,
  saveDescriptionColumn,
  statGroupKey,
  type SortState,
  type StatRow,
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
  it("defaults to on when nothing was stored", () => {
    expect(loadDescriptionColumn(fakeStorage())).toBe(true);
  });

  it("round-trips through storage", () => {
    const storage = fakeStorage();
    saveDescriptionColumn(false, storage);
    expect(storage.getItem(DESCRIPTION_COLUMN_STORAGE_KEY)).toBe("0");
    expect(loadDescriptionColumn(storage)).toBe(false);

    saveDescriptionColumn(true, storage);
    expect(loadDescriptionColumn(storage)).toBe(true);
  });

  it("only an explicit 'off' turns it off", () => {
    expect(loadDescriptionColumn(fakeStorage({ [DESCRIPTION_COLUMN_STORAGE_KEY]: "yes please" }))).toBe(true);
  });

  it("stays on when there is no storage at all", () => {
    expect(loadDescriptionColumn(null)).toBe(true);
    expect(() => saveDescriptionColumn(true, null)).not.toThrow();
  });

  it("survives storage that throws on every access", () => {
    const storage = throwingStorage();
    expect(loadDescriptionColumn(storage)).toBe(true);
    expect(() => saveDescriptionColumn(true, storage)).not.toThrow();
  });
});

describe("nameSegments", () => {
  it("splits a counter name after each separator so it can wrap between words", () => {
    expect(nameSegments("css_filter_total_bytes_saved")).toEqual(["css_", "filter_", "total_", "bytes_", "saved"]);
    expect(nameSegments("pcache-cohorts.dom")).toEqual(["pcache-", "cohorts.", "dom"]);
  });
  it("keeps a name without separators whole", () => {
    expect(nameSegments("flushes")).toEqual(["flushes"]);
    expect(nameSegments("")).toEqual([""]);
  });
});

describe("statGroupKey", () => {
  it("is the text before the first underscore; a bare name groups under itself", () => {
    expect(statGroupKey("cache_hit_ratio")).toBe("cache");
    expect(statGroupKey("ipro_daemon_served")).toBe("ipro");
    expect(statGroupKey("requests")).toBe("requests");
  });
});

describe("groupStatRows", () => {
  const row = (name: string): StatRow => ({ name, value: 0, description: "", delta: null });
  it("curated groups come first, ordered by label; the rest follow, title-cased", () => {
    const groups = groupStatRows([
      row("zzz_unknown_a"), row("javascript_rewrites"), row("cache_hits"),
      row("css_filter_total_bytes_saved"), row("another_new_one"),
    ]);
    expect(groups.map((g) => g.label)).toEqual([
      "Cache", "CSS", "JavaScript", "Another", "Zzz",
    ]);
  });
  it("an unknown prefix is its own honest group; empty input groups nothing", () => {
    expect(groupStatRows([])).toEqual([]);
    expect(groupStatRows([row("total_rewrite_count")]).map((g) => g.label)).toEqual(["Total"]);
  });
  it("shares one label across the curated synonyms", () => {
    expect(STAT_GROUP_LABELS["javascript"]).toBe(STAT_GROUP_LABELS["js"]);
    expect(STAT_GROUP_LABELS["ipro"]).toBe("In-place optimization");
  });
});

describe("buildRows with deltas", () => {
  const sort: SortState = { key: "delta", asc: false };
  it("carries the caller's precomputed deltas; no map entry means null, never NaN", () => {
    // The map's values ARE the deltas (the page computes current − baseline,
    // Step 5a): a rose by 7, b fell by 2, c has no entry → null.
    const rows = buildRows({ a: 10, b: 3, c: 0 }, "", sort, new Map([["a", 7], ["b", -2]]));
    const byName = Object.fromEntries(rows.map((r) => [r.name, r.delta]));
    expect(byName).toEqual({ a: 7, b: -2, c: null });
  });
  it("delta sort puts the biggest change first and rows without a baseline last", () => {
    // Deltas: a = +7, c = 0, b = −2, d = no entry. Descending by delta:
    // a (7), c (0), b (−2); d sits last (nulls last in either direction).
    const rows = buildRows({ a: 10, b: 3, c: 5, d: 5 }, "", { key: "delta", asc: false },
      new Map([["a", 7], ["b", -2], ["c", 0]]));
    expect(rows.map((r) => r.name)).toEqual(["a", "c", "b", "d"]);
    expect(buildRows({ a: 1 }, "", { key: "delta", asc: true }, new Map([["a", 4]]))).toHaveLength(1);
  });
  it("without the parameter every delta is null and value sorting still works", () => {
    const rows = buildRows({ a: 2, b: 1 }, "", DEFAULT_SORT);
    expect(rows.every((r) => r.delta === null)).toBe(true);
    expect(buildRows({ a: 2, b: 1 }, "", { key: "value", asc: false })[0].name).toBe("a");
  });
});

describe("the delta column toggle", () => {
  it("is off by default and persists an explicit choice", () => {
    const store = fakeStorage({ [DELTA_COLUMN_STORAGE_KEY]: "1" });
    expect(loadDeltaColumn(null)).toBe(false); // no storage at all
    expect(loadDeltaColumn(store)).toBe(true); // an explicit "1"
    saveDeltaColumn(true, store);
    expect(store.getItem(DELTA_COLUMN_STORAGE_KEY)).toBe("1");
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import {
  DEFAULT_SORT,
  DELTA_COLUMN_STORAGE_KEY,
  DESCRIPTION_COLUMN_STORAGE_KEY,
  OTHER_FAMILY,
  STAT_FAMILIES,
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
import { NEUTRAL_STAT_DESCRIPTION, STAT_DESCRIPTIONS } from "$lib/data/stat-descriptions";
import statNames from "../../../test-fixtures/stat-names.fixture.json";
import apacheStatNames from "../../../test-fixtures/stat-names.apache-2.8.1.fixture.json";

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

/** Every counter a captured server dump holds, plus every counter the
 * description table knows (some exist on one server type only). */
const KNOWN_NAMES = [
  ...new Set([...(statNames as string[]), ...(apacheStatNames as string[]), ...Object.keys(STAT_DESCRIPTIONS)]),
];

/** Families that may hold a single counter, and why. None today. */
const SINGLE_COUNTER_FAMILIES: Readonly<Record<string, string>> = {};

describe("statGroupKey", () => {
  it("files a counter by what it counts, not by its first word", () => {
    expect(statGroupKey("num_flushes")).toBe("rewriting");
    expect(statGroupKey("num_css_inlined")).toBe("css");
    expect(statGroupKey("num_js_inlined")).toBe("js");
    expect(statGroupKey("num_resource_fetch_failures")).toBe("fetch");
    expect(statGroupKey("ipro_served")).toBe("ipro");
    expect(statGroupKey("ipro_daemon_served")).toBe("optimizer");
    expect(statGroupKey("daemon_html_refused_vary")).toBe("html");
    expect(statGroupKey("named-lock-rewrite-scheduler-granted")).toBe("rewriting");
    expect(statGroupKey("low-priority-worked-queue-depth")).toBe("rewriting");
    expect(statGroupKey("pcache-cohorts-dom_hits")).toBe("pcache");
  });
  it("lets the longest pattern win", () => {
    expect(statGroupKey("cache_hits")).toBe("http");
    expect(statGroupKey("cache_flush_count")).toBe("purge");
    expect(statGroupKey("cache_batcher_queued_gets")).toBe("external");
    expect(statGroupKey("cache_extensions")).toBe("rewriting");
  });
  it("puts an unknown counter in Other", () => {
    expect(statGroupKey("requests")).toBe(OTHER_FAMILY.key);
    expect(statGroupKey("num_brand_new_thing")).toBe(OTHER_FAMILY.key);
    expect(statGroupKey("")).toBe(OTHER_FAMILY.key);
  });
});

describe("the family table", () => {
  it("files every counter a real server emits in a named family", () => {
    const other = KNOWN_NAMES.filter((name) => statGroupKey(name) === OTHER_FAMILY.key);
    expect(other, `counters with no family:\n  ${other.join("\n  ")}`).toEqual([]);
  });

  it("has no family of one, unless it is listed with its reason", () => {
    const sizes = new Map<string, number>();
    for (const name of KNOWN_NAMES) sizes.set(statGroupKey(name), (sizes.get(statGroupKey(name)) ?? 0) + 1);
    for (const [key] of STAT_FAMILIES) {
      const size = sizes.get(key) ?? 0;
      expect(size, `${key} matches no known counter`).toBeGreaterThan(0);
      if (!(key in SINGLE_COUNTER_FAMILIES)) expect(size, `${key} holds one counter`).toBeGreaterThan(1);
    }
  });

  it("gives each family its own key and its own label", () => {
    const keys = STAT_FAMILIES.map(([key]) => key);
    const labels = STAT_FAMILIES.map(([, label]) => label);
    expect(new Set([...keys, OTHER_FAMILY.key]).size).toBe(keys.length + 1);
    expect(new Set([...labels, OTHER_FAMILY.label]).size).toBe(labels.length + 1);
  });

  it("gives no pattern to two families", () => {
    const patterns = STAT_FAMILIES.flatMap(([, , p]) => p.split(" "));
    expect(patterns.filter((p, i) => patterns.indexOf(p) !== i)).toEqual([]);
    expect(patterns).not.toContain("");
  });
});

describe("groupStatRows", () => {
  const row = (name: string): StatRow => ({ name, value: 0, description: "", delta: null });
  it("orders groups by the family table, Other last", () => {
    const groups = groupStatRows([
      row("zzz_unknown_a"), row("javascript_rewrites"), row("cache_hits"),
      row("css_filter_total_bytes_saved"), row("num_flushes"), row("ipro_served"),
    ]);
    expect(groups.map((g) => g.label)).toEqual([
      "Page rewriting", "In-place optimization", "HTTP cache", "CSS", "JavaScript", "Other",
    ]);
  });
  it("groups nothing for no rows, and keeps the caller's row order", () => {
    expect(groupStatRows([])).toEqual([]);
    const groups = groupStatRows([row("num_js_inlined"), row("javascript_minify_uses"), row("js_file_count_reduction")]);
    expect(groups).toHaveLength(1);
    expect(groups[0].rows.map((r) => r.name)).toEqual(["num_js_inlined", "javascript_minify_uses", "js_file_count_reduction"]);
  });
  it("keeps one key per family whatever the row order", () => {
    const rows = [row("js_a"), row("javascript_b"), row("num_js_inlined")];
    expect(groupStatRows(rows).map((g) => g.key)).toEqual(["js"]);
    expect(groupStatRows([...rows].reverse()).map((g) => g.key)).toEqual(["js"]);
  });
  it("splits the whole-server dump into a few named families", () => {
    const groups = groupStatRows((apacheStatNames as string[]).map(row));
    expect(groups.map((g) => g.key)).not.toContain(OTHER_FAMILY.key);
    expect(groups.length).toBeLessThanOrEqual(STAT_FAMILIES.length);
    expect(groups.reduce((n, g) => n + g.rows.length, 0)).toBe((apacheStatNames as string[]).length);
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

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { RulePackDecisionRow, RulePacksResponse } from "$lib/api/types";
import {
  NO_FILTERS,
  distinctValues,
  filterRows,
  packStateInfo,
  rulePacksView,
  tailPackOptions,
  tailStatusText,
  withQuery,
} from "./rule-packs";

const base: RulePacksResponse = {
  schema: 1,
  scope: "vhost",
  host: "t.test",
  generated_at: "2026-10-11T12:00:05Z",
  configuration: { filter_enabled: true, rule_pack_files: ["/p/a.json"], rule_pack_mode: "report", rule_pack_require_signed: false },
  packs: [],
  overlap_warnings: [],
  counters: { rule_pack_pages_evaluated: 3, rule_pack_actions_applied: 1 },
  decisions: { date: "2026-10-11", pack: null, limit: 100, status: "ok", error: null, rows: [] },
};

describe("rulePacksView", () => {
  it("refuses a schema it does not know", () => {
    expect(() => rulePacksView({ ...base, schema: 2 })).toThrow(/schema/);
    expect(() => rulePacksView({})).toThrow(/schema/);
  });

  it("a configured file the server has not read is 'not loaded yet', not 'none configured'", () => {
    const v = rulePacksView({ ...base, packs: [{ position: 0, file: "/p/a.json", state: "none", id: null }] });
    expect(v.notLoadedYet).toBe(true);
    expect(v.noneConfigured).toBe(false);
    expect(v.packs[0].state.label).toBe("not read yet");
  });

  it("no files at all is 'none configured'", () => {
    const v = rulePacksView({ ...base, configuration: { ...base.configuration, rule_pack_files: [] } });
    expect(v.noneConfigured).toBe(true);
    expect(v.notLoadedYet).toBe(false);
  });

  it("a loaded pack is not 'not loaded yet'", () => {
    const v = rulePacksView({ ...base, packs: [{ file: "/p/a.json", state: "loaded", id: "edge-seo", version: "0.1.0" }] });
    expect(v.notLoadedYet).toBe(false);
    expect(v.packs[0].state.tone).toBe("success");
  });

  it("sorts the counters and keeps explicit nulls", () => {
    const v = rulePacksView(base);
    expect(v.counters?.map((c) => c.name)).toEqual(["rule_pack_actions_applied", "rule_pack_pages_evaluated"]);
    expect(rulePacksView({ ...base, counters: null }).counters).toBeNull();
  });

  it("reads a licence as metadata text and a missing one as null", () => {
    const v = rulePacksView({
      ...base,
      packs: [
        { file: "a", state: "loaded", id: "a", license: { label: "Example", expires: "2027-01-01", terms: null } },
        { file: "b", state: "loaded", id: "b", license: null },
      ],
    });
    expect(v.packs[0].license).toEqual({ label: "Example", expires: "2027-01-01", terms: null });
    expect(v.packs[1].license).toBeNull();
  });
});

describe("packStateInfo", () => {
  it("names every state the module reports", () => {
    expect(packStateInfo("loaded", true).tone).toBe("success");
    expect(packStateInfo("failed-keeping-earlier", true).tone).toBe("warning");
    expect(packStateInfo("refused", false).tone).toBe("error");
    expect(packStateInfo("duplicate-id", true).label).toBe("duplicate id");
    expect(packStateInfo("none", true).label).toBe("not loaded");
    expect(packStateInfo("future-state", true).label).toBe("future-state");
  });
});

describe("tail", () => {
  const rows: RulePackDecisionRow[] = [
    { pack_id: "edge-seo", action: "insert", reason: "missing", rule: "canonical-keep", host: "A.test" },
    { pack_id: "edge-seo", action: "none", reason: "present", rule: "title-keep", host: "b.test" },
    { pack_id: "house", action: "insert", reason: "missing", rule: "Robots-meta", host: "a.test" },
  ];

  it("filters by pack, action, reason, rule (substring, any case) and host", () => {
    expect(filterRows(rows, NO_FILTERS)).toHaveLength(3);
    expect(filterRows(rows, { ...NO_FILTERS, pack: "house" })).toHaveLength(1);
    expect(filterRows(rows, { ...NO_FILTERS, action: "insert" })).toHaveLength(2);
    expect(filterRows(rows, { ...NO_FILTERS, reason: "present" })).toHaveLength(1);
    expect(filterRows(rows, { ...NO_FILTERS, rule: "ROBOTS" })).toHaveLength(1);
    expect(filterRows(rows, { ...NO_FILTERS, host: "a.test" })).toHaveLength(2);
    expect(filterRows(rows, { ...NO_FILTERS, action: "insert", pack: "edge-seo" })).toHaveLength(1);
  });

  it("offers the distinct values sorted", () => {
    expect(distinctValues(rows, "pack_id")).toEqual(["edge-seo", "house"]);
    expect(distinctValues(rows, "action")).toEqual(["insert", "none"]);
  });

  it("explains why the tail is empty", () => {
    expect(tailStatusText("ok", null)).toBeNull();
    expect(tailStatusText("missing", null)).toMatch(/No decision log/);
    expect(tailStatusText("log_dir_not_set", null)).toMatch(/LogDir/);
    expect(tailStatusText("error", "denied")).toMatch(/denied/);
  });

  it("lists the loadable pack ids once, in load order", () => {
    const v = rulePacksView({
      ...base,
      packs: [
        { file: "a", state: "loaded", id: "x" },
        { file: "b", state: "duplicate-id", id: "x" },
        { file: "c", state: "none", id: null },
        { file: "d", state: "loaded", id: "y" },
      ],
    });
    expect(tailPackOptions(v.packs)).toEqual(["x", "y"]);
  });
});

describe("withQuery", () => {
  it("adds only the parameters that are set", () => {
    expect(withQuery("/a/rule_packs", { raw: "1", pack: "edge seo" })).toBe("/a/rule_packs?raw=1&pack=edge+seo");
    expect(withQuery("/a/rule_packs", { raw: "1", pack: null })).toBe("/a/rule_packs?raw=1");
    expect(withQuery("/a/rule_packs", { pack: null })).toBe("/a/rule_packs");
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { ALERT_RULES, AlertTracker, RATE_ALERT_HOLD_MS, type AlertSnapshot } from "$lib/alerts";
import { ALREADY_GOOD, DUE_TO_LOAD, NOT_OPTIMIZED, reasonRows } from "./not-optimized";
import { WARMUP_MS } from "./since";

const AT = 2_000_000_000_000;
const STEADY = AT - WARMUP_MS - 60_000;
const NAMES: string[] = JSON.parse(
  readFileSync(new URL("../../../test-fixtures/stat-names.apache-2.8.1.fixture.json", import.meta.url), "utf8"),
);

function snap(module: Record<string, number> | null, at = AT): AlertSnapshot {
  return { module, daemon: "ok", health: null, daemonStats: null, daemonStatsUnsupported: false, belowFloor: false, at };
}

describe("not-optimized reasons", () => {
  it("name only counters an Apache server reports", () => {
    for (const r of [...NOT_OPTIMIZED, ...ALREADY_GOOD]) expect(NAMES, r.counter).toContain(r.counter);
  });

  it("lists the reasons counted, in table order, and leaves out a zero or missing counter", () => {
    const vars = { process_start_ms: STEADY, css_filter_parse_failures: 3, resource_url_domain_rejections: 96, javascript_minification_failures: 0 };
    expect(reasonRows(vars, AT, NOT_OPTIMIZED)!.map((r) => [r.reason.counter, r.count])).toEqual([
      ["resource_url_domain_rejections", 96],
      ["css_filter_parse_failures", 3],
    ]);
    expect(reasonRows({ ...vars, image_rewrites_dropped_nosaving_noresize: 4 }, AT, ALREADY_GOOD)!.map((r) => r.count)).toEqual([4]);
  });

  it("says nothing in warm-up or without a module start", () => {
    expect(reasonRows({ process_start_ms: AT - 60_000, css_filter_parse_failures: 3 }, AT, NOT_OPTIMIZED)).toBeNull();
    expect(reasonRows({ css_filter_parse_failures: 3 }, AT, NOT_OPTIMIZED)).toBeNull();
    expect(reasonRows(null, AT, NOT_OPTIMIZED)).toBeNull();
  });
});

describe("not-optimized findings", () => {
  const rule = ALERT_RULES.find((r) => r.id === "image-work-dropped")!;
  const at = (n: number, start = STEADY, t = AT) => snap({ process_start_ms: start, [DUE_TO_LOAD]: n }, t);

  it("only image work dropped under load is a finding; the other reasons are context on Savings", () => {
    const ids = new Set(ALERT_RULES.map((r) => r.id));
    for (const r of [...NOT_OPTIMIZED, ...ALREADY_GOOD]) expect(ids.has(r.counter.replace(/_/g, "-"))).toBe(false);
    expect(rule.kind).toBe("rate");
    expect(rule.severity).toBe("warning");
    expect(rule.link).toBe("#/savings");
  });

  it("fires on a rise past warm-up; silent in warm-up, on a missing counter and without a previous sample", () => {
    expect(rule.check(at(14, STEADY, AT + 5_000), at(12), [])).toBe(true);
    expect(rule.check(at(12, STEADY, AT + 5_000), at(12), [])).toBe(false);
    expect(rule.check(at(14), null, [])).toBeUndefined();
    const warm = AT - 60_000;
    expect(rule.check(at(14, warm, AT + 5_000), at(12, warm), [])).toBeUndefined();
    expect(rule.check(snap({ process_start_ms: STEADY }, AT + 5_000), snap({ process_start_ms: STEADY }), [])).toBeUndefined();
  });

  it("states the rise, the count since the start, and clears after the hold so an acknowledgement re-arms", () => {
    const t = new AlertTracker();
    t.evaluate(at(12), AT);
    const out = t.evaluate(at(14, STEADY, AT + 5_000), AT + 5_000);
    expect(out.find((a) => a.id === "image-work-dropped")?.message).toBe(
      "The server was too busy to optimize 2 images since the last refresh (14 since the web server started); those are served as they are.",
    );
    t.acknowledge("image-work-dropped");
    const later = AT + 5_000 + RATE_ALERT_HOLD_MS + 5_000;
    expect(t.evaluate(at(14, STEADY, later), later).some((a) => a.id === "image-work-dropped")).toBe(false);
    const again = later + 5_000;
    expect(t.evaluate(at(15, STEADY, again), again).some((a) => a.id === "image-work-dropped")).toBe(true);
  });
});

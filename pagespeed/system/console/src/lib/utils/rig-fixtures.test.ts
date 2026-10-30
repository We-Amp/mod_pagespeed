// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The page models, fed the REAL JSON of every console leaf.
//
// test-fixtures/rig/ holds what a local Apache + module + optimizer stack
// answered after a controlled workload (tools/console-audit/rig.sh, refreshed
// by scripts/refresh-stat-fixture.sh --rig). Two vhosts served the same
// fixture set a different number of times, so the per-host rows differ. The
// expected numbers below come from two places, never from the model under
// test: the client's own bookkeeping (expected.json) and arithmetic on the raw
// leaves. A model that drops a host row, counts one twice, or reads the wrong
// counter fails here.
//
// Mocks written from a plan reproduce the plan's assumptions; these do not.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { cacheHitRate, daemonSavedRaw, optimizedCopyHitRate, savingsSplit } from "./savings";
import { formatPercent } from "./format";
import { graphsView } from "./graphs-view";
import { hostRow, hostSavingsView, siteRow } from "./host-savings";
import { daemonServeSavings, moduleSummary, sampleOverview, variablesOf } from "./overview";
import type { OverviewApi } from "./overview";
import { rulePacksView } from "./rule-packs";
import { serveSavingsView } from "./serve-savings";

const dir = fileURLToPath(new URL("../../../test-fixtures/rig/", import.meta.url));
// eslint-disable-next-line @typescript-eslint/no-explicit-any
const load = (name: string): any => JSON.parse(readFileSync(dir + name + ".json", "utf8"));

const daemonGlobal = load("daemon_stats.global");
const daemonA = load("daemon_stats.vhost-a");
const daemonB = load("daemon_stats.vhost-b");
const statsGlobal = load("stats_json.global");
const expected = load("expected");

const CLASSES = ["css", "js", "image"] as const;
const sum = (xs: number[]) => xs.reduce((a, b) => a + b, 0);

describe("the fixtures are a real, busy capture", () => {
  it("every leaf answered 200", () => {
    const manifest = load("manifest") as Record<string, { status: number; file: string }>;
    for (const [name, m] of Object.entries(manifest)) {
      expect(m.status, name).toBe(200);
      expect(m.file.endsWith(".json"), name).toBe(true);
    }
  });

  it("both vhosts served optimized responses, so the rows are not vacuous", () => {
    const view = hostSavingsView(daemonGlobal.serve_savings_by_host)!;
    expect(view.hosts.map((r) => r.host).sort()).toEqual(["site-a.test", "site-b.test"]);
    expect(view.hosts.every((r) => r.hits > 100)).toBe(true);
  });
});

describe("hostSavingsView: per-host rows add up to the totals", () => {
  const view = hostSavingsView(daemonGlobal.serve_savings_by_host)!;
  const classes = Object.values(daemonGlobal.serve_savings) as Array<{
    hits: number;
    original_bytes: number;
    optimized_bytes: number;
  }>;
  const total = {
    hits: sum(classes.map((c) => c.hits)),
    original: sum(classes.map((c) => c.original_bytes)),
    served: sum(classes.map((c) => c.optimized_bytes)),
  };

  it("host rows plus 'other' equal the per-class totals (hits, original, served)", () => {
    const rows = [...view.hosts, ...(view.other ? [view.other] : [])];
    expect(sum(rows.map((r) => r.hits))).toBe(total.hits);
    expect(sum(rows.map((r) => r.original))).toBe(total.original);
    expect(sum(rows.map((r) => r.served))).toBe(total.served);
  });

  it("each row carries exactly the counters the optimizer reported for that host", () => {
    for (const raw of daemonGlobal.serve_savings_by_host.hosts) {
      const row = hostRow(view, raw.host)!;
      expect(row.hits).toBe(raw.hits);
      expect(row.original).toBe(raw.original_bytes);
      expect(row.served).toBe(raw.optimized_bytes);
      expect(row.saved).toBe(Math.max(0, raw.original_bytes - raw.optimized_bytes));
    }
  });

  it("the busier vhost (twice the requests) sorts first", () => {
    expect(view.hosts[0].host).toBe("site-b.test");
    expect(view.hosts[0].hits).toBeGreaterThan(view.hosts[1].hits);
  });

  it("a per-vhost console's narrowed answer shows its own site and files the rest under 'other'", () => {
    for (const [daemon, site, rest] of [
      [daemonA, "site-a.test", "site-b.test"],
      [daemonB, "site-b.test", "site-a.test"],
    ] as const) {
      const v = hostSavingsView(daemon.serve_savings_by_host)!;
      const own = siteRow(v, site);
      expect(own.host).toBe(site);
      expect(own.row?.hits).toBe(hostRow(view, site)!.hits);
      expect(v.other?.hits).toBe(hostRow(view, rest)!.hits);
      // The one host row plus 'other' still add up to the whole server.
      const rows = [...v.hosts, ...(v.other ? [v.other] : [])];
      expect(sum(rows.map((r) => r.hits))).toBe(total.hits);
      expect(sum(rows.map((r) => r.original))).toBe(total.original);
      expect(sum(rows.map((r) => r.served))).toBe(total.served);
      // Whole-server totals are the same on every console.
      expect(sum(Object.values(daemon.serve_savings as Record<string, { hits: number }>).map((c) => c.hits))).toBe(
        total.hits,
      );
    }
  });

  // Records CURRENT behaviour (a HEAD request is counted as a full serve)
  // pending a product decision.
  it("the client's own request count explains each host's hits", () => {
    // Per optimizable file and vhost: GETs in three encodings plus one HEAD are
    // counted as optimizer serves; a conditional request (304) is not.
    const k = (v: string) => expected.rounds * (v === "b" ? 2 : 1);
    const hostOf: Record<string, string> = { a: "site-a.test", b: "site-b.test" };
    const clientHits: Record<string, number> = {};
    for (const [key, w] of Object.entries(expected.warm) as Array<[string, { original: number; settled_bytes: number }]>) {
      const [vhost, file] = key.split(":");
      if (!(w.settled_bytes < w.original)) continue; // not optimized: served as the origin's bytes
      expect(CLASSES as readonly string[]).toContain(expected.assets.type[file]);
      clientHits[hostOf[vhost]] = (clientHits[hostOf[vhost]] ?? 0) + 3 * k(vhost) + 1;
    }
    for (const host of Object.values(hostOf)) {
      const row = hostRow(view, host)!;
      // The lifetime rows also hold the warm-up serves and earlier runs, so
      // they are at least what this client sent; the exact window is checked
      // against the two snapshots below.
      expect(row.hits).toBeGreaterThanOrEqual(clientHits[host]);
    }
  });

  it("malformed or duplicated rows cannot break the sum (the model's invariant on real rows)", () => {
    const dup = {
      ...daemonGlobal.serve_savings_by_host,
      hosts: [...daemonGlobal.serve_savings_by_host.hosts, ...daemonGlobal.serve_savings_by_host.hosts],
    };
    const doubled = hostSavingsView(dup)!;
    // The optimizer sums a repeated host itself; the view does the same.
    expect(doubled.hosts.map((r) => [r.hits, r.original, r.served])).toEqual(
      view.hosts.map((r) => [r.hits * 2, r.original * 2, r.served * 2]),
    );
  });
});

describe("serveSavingsView on the real serve_savings block", () => {
  const view = serveSavingsView(daemonGlobal.serve_savings)!;

  it("lists CSS, JavaScript and Images as served and HTML as not served", () => {
    expect(view.served.map((r) => r.key)).toEqual(["css", "js", "image"]);
    expect(view.notServed).toEqual(["HTML"]);
  });

  it("each class equals its raw counters, with saved = original - served", () => {
    for (const row of view.served) {
      const raw = daemonGlobal.serve_savings[row.key];
      expect(row.hits).toBe(raw.hits);
      expect(row.original).toBe(raw.original_bytes);
      expect(row.served).toBe(raw.optimized_bytes);
      expect(row.saved).toBe(raw.original_bytes - raw.optimized_bytes);
    }
  });

  it("the total is the sum of the class rows and agrees with the overview and the raw saved sum", () => {
    const original = sum(view.served.map((r) => r.original));
    const saved = sum(view.served.map((r) => r.saved));
    expect(view.total).toEqual({ saved, original, percent: Math.round((saved / original) * 100) });
    expect(daemonServeSavings(daemonGlobal)).toEqual(view.total);
    expect(daemonSavedRaw(daemonGlobal.serve_savings)).toBe(saved);
  });

  // Documents a gap: in this rig the optimizer recorded every serve as identity
  // (Apache compressed downstream via mod_deflate and the rig has no brotli
  // module), so the br/gzip rows are zero and the sums below are trivially
  // satisfied. It becomes meaningful once the optimizer serves stored encodings.
  it("serves by encoding add up to the class's hits (all identity in this rig)", () => {
    for (const cls of CLASSES) {
      const enc = daemonGlobal.serve_savings[cls].by_encoding;
      expect(enc.gzip.hits + enc.br.hits, `${cls} compressed serves`).toBe(0);
    }
    for (const cls of CLASSES) {
      const enc = daemonGlobal.serve_savings[cls].by_encoding as Record<string, { hits: number; bytes: number }>;
      expect(sum(Object.values(enc).map((e) => e.hits))).toBe(daemonGlobal.serve_savings[cls].hits);
    }
    for (const split of savingsSplit(daemonGlobal.serve_savings, daemonGlobal.verdicts)) {
      const raw = daemonGlobal.serve_savings[split.key];
      expect(split.totalServes).toBe(raw.hits);
      expect(split.summary?.saved).toBe(view.served.find((r) => r.key === split.key)!.saved);
    }
  });

  it("the measured window equals what the client sent (hits and bytes saved per class)", () => {
    const before = expected.before.optimizer.serve_savings;
    const after = expected.after.optimizer.serve_savings;
    const k = (v: string) => expected.rounds * (v === "b" ? 2 : 1);
    const hits: Record<string, number> = { css: 0, js: 0, image: 0 };
    const saved: Record<string, number> = { css: 0, js: 0, image: 0 };
    for (const [key, w] of Object.entries(expected.warm) as Array<[string, { original: number; settled_bytes: number }]>) {
      const [vhost, file] = key.split(":");
      if (!(w.settled_bytes < w.original)) continue;
      const cls = expected.assets.type[file] as string;
      const n = 3 * k(vhost) + 1; // three encodings of K GETs, plus the HEAD
      hits[cls] += n;
      saved[cls] += n * (w.original - w.settled_bytes);
    }
    for (const cls of CLASSES) {
      expect(after[cls].hits - before[cls].hits, `${cls} hits`).toBe(hits[cls]);
      expect(
        after[cls].original_bytes - after[cls].optimized_bytes - (before[cls].original_bytes - before[cls].optimized_bytes),
        `${cls} saved bytes`,
      ).toBe(saved[cls]);
    }
  });
});

describe("module counters", () => {
  const vars = variablesOf(statsGlobal);

  it("the optimized-copy hit rate is served / (served + original + per-class fallthrough)", () => {
    const served = vars.ipro_daemon_served;
    const classSum =
      vars.ipro_daemon_fallthrough_css + vars.ipro_daemon_fallthrough_js + vars.ipro_daemon_fallthrough_image;
    const r = optimizedCopyHitRate(vars);
    expect(r.mode).toBe("optimizable");
    expect(r.served).toBe(served);
    expect(r.total).toBe(served + vars.ipro_daemon_served_original + classSum);
    expect(r.percent).toBe(Math.round((served / r.total) * 100));
    const all = cacheHitRate(vars);
    expect(all.total).toBe(served + vars.ipro_daemon_served_original + vars.ipro_daemon_fallthrough);
  });

  it("the module and the optimizer agree on how many responses came from the optimizer", () => {
    expect(vars.ipro_daemon_served).toBe(daemonGlobal.serve_classes.optimized);
    expect(vars.ipro_daemon_served).toBe(
      vars.ipro_daemon_served_css + vars.ipro_daemon_served_js + vars.ipro_daemon_served_image + vars.ipro_daemon_served_other,
    );
    expect(moduleSummary(vars).servedByOptimizer).toBe(vars.ipro_daemon_served);
  });

  // Records CURRENT behaviour (the optimizer does not count a 304) pending a
  // product decision.
  it("in the measured window the module's per-class served counters = the optimizer's hits + the 304s", () => {
    // The module counts a conditional request answered 304 as an optimizer
    // serve; the optimizer records serve bytes for full responses only. The
    // difference is therefore exactly the conditional requests the client sent
    // for optimized files (one per file and vhost). The page that shows both
    // figures must not sum them.
    const m0 = expected.before.module.global;
    const m1 = expected.after.module.global;
    const o0 = expected.before.optimizer.serve_savings;
    const o1 = expected.after.optimizer.serve_savings;
    const conditional: Record<string, number> = { css: 0, js: 0, image: 0 };
    for (const [key, w] of Object.entries(expected.warm) as Array<[string, { original: number; settled_bytes: number }]>) {
      const file = key.split(":")[1];
      if (w.settled_bytes < w.original) conditional[expected.assets.type[file]] += 1;
    }
    expect(expected.conditional.not_modified).toBe(expected.conditional.requests);
    for (const cls of CLASSES) {
      const module = m1[`ipro_daemon_served_${cls}`] - m0[`ipro_daemon_served_${cls}`];
      const optimizer = o1[cls].hits - o0[cls].hits;
      expect(module - optimizer, cls).toBe(conditional[cls]);
    }
  });

  it("the overview treats a server with traffic as active", () => {
    expect(moduleSummary(vars).active).toBe(true);
  });
});

describe("empty and missing data read as no data, not as zero", () => {
  it("0 / 0 renders as a dash", () => {
    expect(cacheHitRate({}).percent).toBeNull();
    expect(formatPercent(cacheHitRate({}).percent)).toBe("—");
    expect(hostSavingsView({ hosts: [], other: { hits: 0, original_bytes: 0, optimized_bytes: 0 } })!.other).toBeNull();
    const zeroed = JSON.parse(JSON.stringify(daemonGlobal.serve_savings));
    for (const c of Object.values(zeroed) as Array<Record<string, unknown>>) {
      c.hits = 0;
      c.original_bytes = 0;
      c.optimized_bytes = 0;
    }
    expect(serveSavingsView(zeroed)!.total).toBeNull();
  });
});

describe("the other leaves parse into their page models", () => {
  it("graphs: every series is aligned with the timestamps", () => {
    const view = graphsView(load("graphs.global"));
    expect(view.kind).toBe("data");
    if (view.kind !== "data") return;
    expect(view.graphs.length).toBeGreaterThan(0);
    for (const g of view.graphs) expect(g.data.length, g.name).toBe(view.timestamps.length);
  });

  it("rule packs: the global and the vhost answers are told apart and name no packs by default", () => {
    const g = rulePacksView(load("rule_packs.global"));
    const v = rulePacksView(load("rule_packs.vhost-a"));
    expect(g.scope).not.toBe(v.scope);
    expect(g.noneConfigured).toBe(true);
    expect(g.packs).toEqual([]);
    expect(v.counters?.length).toBe(g.counters?.length);
  });

  it("overview: a sample taken from the real leaves has every read ok", async () => {
    const api = {
      getStats: async () => statsGlobal,
      daemonHealth: async () => load("daemon_health.global"),
      daemonStats: async () => daemonGlobal,
      getMessageGroups: async () => load("message_history.grouped.global"),
    } as unknown as OverviewApi;
    const s = await sampleOverview(api, () => 1);
    expect(s.module.ok && s.health.ok && s.daemonStats.ok && s.messages?.ok).toBe(true);
  });
});

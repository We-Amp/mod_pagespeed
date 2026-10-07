// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { DaemonHealthResponse, DaemonStatsResponse } from "$lib/api/types";
import { EMPTY_MESSAGE_TEXT, type MessageGroup } from "$lib/utils/message-groups";
import { createAckStore, memoryAckStore } from "$lib/utils/finding-acks";
import { formatClock } from "$lib/utils/format";
import { messageTemplate } from "$lib/utils/message-template";
import { WARMUP_MS } from "$lib/utils/since";
import {
  ALERT_RULES,
  AlertTracker,
  MAX_HISTORY,
  RATE_ALERT_HOLD_MS,
  VOLUME_WINDOW_MS,
  failedChecks,
  healthSummary,
  num,
  rankAlerts,
  type ActiveAlert,
  type AlertRule,
  type AlertSeverity,
  type AlertSnapshot,
} from "./alerts";

const T0 = 1_000_000;

function snap(over: Partial<AlertSnapshot> = {}): AlertSnapshot {
  return {
    module: { num_resource_fetch_failures: 0 },
    daemon: "ok",
    health: {
      status: "ok",
      ready: true,
      version: "2.0.41",
      checks: { cache_configured: { pass: true }, cache_open: { pass: true } },
      browser: { enabled: true, chrome_running: true },
      connections: { active: 1, max: 32 },
    },
    daemonStats: {
      errors: { total: 0, origin_misconfiguration: 0 },
      alternates: { write_failures: 0, writes: 10 },
      thread_pool: { inflight: 0, size: 2 },
      connections: { active: 1, max: 32 },
    },
    daemonStatsUnsupported: false,
    belowFloor: false,
    ...over,
  };
}

/** A healthy snapshot with some daemon-stats blocks replaced. */
function withStats(patch: Record<string, unknown>): AlertSnapshot {
  const base = snap();
  return snap({ daemonStats: { ...base.daemonStats, ...patch } as DaemonStatsResponse });
}

const errors = (total: number, origin = 0) =>
  withStats({ errors: { total, origin_misconfiguration: origin } });
const ids = (alerts: ActiveAlert[]) => alerts.map((a) => a.id);
const absent = (daemon: AlertSnapshot["daemon"]) =>
  snap({ daemon, health: null, daemonStats: null });

describe("ALERT_RULES", () => {
  it("has unique ids and a label, severity, kind and fix on every rule", () => {
    const seen = new Set<string>();
    for (const rule of ALERT_RULES) {
      expect(seen.has(rule.id)).toBe(false);
      seen.add(rule.id);
      expect(rule.id).toMatch(/^[a-z][a-z0-9-]{0,63}$/);
      expect(rule.label.length).toBeGreaterThan(0);
      expect(["error", "warning", "info"]).toContain(rule.severity);
      expect(["rate", "state"]).toContain(rule.kind);
      expect(rule.fix.length, rule.id).toBeGreaterThan(0);
      if (rule.doc !== undefined) expect(rule.doc).toMatch(/^https:\/\/modpagespeed\.com\/1\.1\/docs\//);
    }
    expect(seen.size).toBe(16);
  });

  it("fix texts name the pages the sidebar has", () => {
    for (const rule of ALERT_RULES) {
      expect(rule.fix, rule.id).not.toMatch(/Daemon status|Optimizer Logs|\bMessages\b/i);
    }
    const fix = (id: string) => ALERT_RULES.find((r) => r.id === id)?.fix;
    expect(fix("daemon-check-failed")).toBe("Optimizer status shows each check; the optimizer's log usually names the cause.");
    expect(fix("error-rate")).toBe("Logs (the optimizer's entries) lists the errors; most name the URL and the step that failed.");
    expect(fix("fetch-failures")).toBe("Check that the web server itself can reach the origin for these URLs; Logs lists them.");
    expect(fix("recent-warnings")).toBe(
      "Logs lists them grouped by kind, newest first; the kinds that need action have their own findings here.",
    );
  });
});

describe("num", () => {
  it("reads a finite number at a dotted path", () => {
    expect(num({ a: { b: 3 } }, "a.b")).toBe(3);
    expect(num({ flat_name: 2 }, "flat_name")).toBe(2);
  });
  it("is undefined for missing, non-numeric, non-finite or non-object inputs", () => {
    expect(num(null, "a")).toBeUndefined();
    expect(num({ a: "3" }, "a")).toBeUndefined();
    expect(num({ a: Number.NaN }, "a")).toBeUndefined();
    expect(num({ a: [1] }, "a.0")).toBeUndefined();
    expect(num("text", "length")).toBeUndefined();
  });
});

describe("AlertTracker", () => {
  it("raises nothing for a healthy optimizer and module", () => {
    const t = new AlertTracker();
    expect(t.evaluate(snap(), T0)).toEqual([]);
    expect(t.evaluate(snap(), T0 + 5000)).toEqual([]);
  });

  it("never fires a rate rule on the first sample", () => {
    const t = new AlertTracker();
    expect(ids(t.evaluate(errors(5), T0))).toEqual([]);
  });

  it("fires error-rate when optimizer errors increase between samples", () => {
    const t = new AlertTracker();
    t.evaluate(errors(1), T0);
    const out = t.evaluate(errors(4), T0 + 5000);
    expect(ids(out)).toEqual(["error-rate"]);
    expect(out[0].message).toBe("The optimizer reported 3 new errors (4 since it started).");
    expect(out[0].severity).toBe("warning");
    expect(out[0].since).toBe(T0 + 5000);
  });

  it("holds a rate alert for the hold window after the last increase, then clears it", () => {
    const t = new AlertTracker();
    t.evaluate(errors(1), T0);
    t.evaluate(errors(2), T0 + 5000);
    expect(ids(t.evaluate(errors(2), T0 + 5000 + RATE_ALERT_HOLD_MS - 1))).toEqual(["error-rate"]);
    expect(ids(t.evaluate(errors(2), T0 + 5000 + RATE_ALERT_HOLD_MS))).toEqual([]);
  });

  it("a counter that went backwards (restart) is not an increase", () => {
    const t = new AlertTracker();
    t.evaluate(errors(10), T0);
    expect(ids(t.evaluate(errors(2), T0 + 5000))).toEqual([]);
    expect(ids(t.evaluate(errors(3), T0 + 10000))).toEqual(["error-rate"]);
  });

  it("write failures are an error and rank before warnings", () => {
    const t = new AlertTracker();
    t.evaluate(withStats({ alternates: { write_failures: 0 }, errors: { total: 0, origin_misconfiguration: 1 } }), T0);
    const out = t.evaluate(
      withStats({ alternates: { write_failures: 2 }, errors: { total: 0, origin_misconfiguration: 1 } }),
      T0 + 5000,
    );
    expect(ids(out)).toEqual(["write-failures", "origin-misconfiguration"]);
    expect(out[0].severity).toBe("error");
    expect(out[0].message).toContain("could not write 2 optimized variants (2 since it started)");
  });

  it("state rules: origin, threads, connections, browser", () => {
    const cases: Array<[AlertSnapshot, string]> = [
      [errors(0, 3), "origin-misconfiguration"],
      [withStats({ thread_pool: { inflight: 2, size: 2 } }), "thread-saturation"],
      [withStats({ connections: { active: 30, max: 32 } }), "connection-saturation"],
      [snap({ health: { status: "ok", ready: true, browser: { enabled: true, chrome_running: false } } }), "browser-stopped"],
    ];
    for (const [s, id] of cases) {
      expect(ids(new AlertTracker().evaluate(s, T0))).toEqual([id]);
    }
    // Boundaries that must stay quiet.
    expect(ids(new AlertTracker().evaluate(withStats({ thread_pool: { inflight: 0, size: 0 } }), T0))).toEqual([]);
    expect(ids(new AlertTracker().evaluate(withStats({ connections: { active: 28, max: 32 } }), T0))).toEqual([]);
    expect(ids(new AlertTracker().evaluate(withStats({ connections: { active: 0, max: 0 } }), T0))).toEqual([]);
    expect(
      ids(new AlertTracker().evaluate(snap({ health: { status: "ok", browser: { enabled: false, chrome_running: false } } }), T0)),
    ).toEqual([]);
  });

  it("names a failing health check and links to Optimizer status", () => {
    const out = new AlertTracker().evaluate(
      snap({ health: { status: "ok", checks: { cache_configured: { pass: true }, cache_open: { pass: false } } } }),
      T0,
    );
    expect(ids(out)).toEqual(["daemon-check-failed"]);
    expect(out[0].message).toBe("Optimizer health check failing: cache_open.");
    expect(out[0].link).toBe("#/optimizer");
  });

  it("missing or malformed daemon blocks: silent, no throw", () => {
    const t = new AlertTracker();
    expect(() => t.evaluate(snap({ health: {}, daemonStats: {} }), T0)).not.toThrow();
    expect(ids(t.evaluate(snap({ health: {}, daemonStats: {} }), T0 + 5000))).toEqual([]);
    const junk = snap({
      health: { checks: "all good" as unknown as Record<string, unknown>, browser: "on" as unknown as { enabled?: boolean } },
      daemonStats: { errors: "none", thread_pool: { inflight: "2", size: 2 }, connections: null } as unknown as AlertSnapshot["daemonStats"],
    });
    expect(ids(t.evaluate(junk, T0 + 10000))).toEqual([]);
  });

  it("daemon unreachable raises one warning; the daemon-derived rules are silent", () => {
    const out = new AlertTracker().evaluate(absent("unreachable"), T0);
    expect(ids(out)).toEqual(["daemon-unreachable"]);
    expect(out[0].link).toBe("#/optimizer");
  });

  it("a server without an optimizer (not configured) raises nothing", () => {
    expect(new AlertTracker().evaluate(absent("not-configured"), T0)).toEqual([]);
  });

  it("below the floor: an optimizer too old for this console raises the update warning", () => {
    expect(ids(new AlertTracker().evaluate(absent("unsupported"), T0))).toEqual(["daemon-outdated"]);
    const out = new AlertTracker().evaluate(snap({ daemonStats: null, daemonStatsUnsupported: true }), T0);
    expect(ids(out)).toEqual(["daemon-outdated"]);
    expect(out[0].message).toContain("(2.0.41) does not provide");
  });

  it("below the floor by version: the same warning, naming the version", () => {
    const old = snap({ belowFloor: true, health: { status: "ok", ready: true, version: "2.0.3" } });
    const out = new AlertTracker().evaluate(old, T0);
    expect(ids(out)).toEqual(["daemon-outdated"]);
    expect(out[0].message).toContain("(2.0.3) is older than the oldest this console supports");
  });

  it("module fetch failures fire on an increase and link to the module's Logs", () => {
    const t = new AlertTracker();
    t.evaluate(snap({ module: { num_resource_fetch_failures: 1 } }), T0);
    const out = t.evaluate(snap({ module: { num_resource_fetch_failures: 3 } }), T0 + 5000);
    expect(ids(out)).toEqual(["fetch-failures"]);
    expect(out[0].link).toBe("#/logs?source=module&level=warning");
    // A scope whose statistics do not report the counter stays silent.
    const u = new AlertTracker();
    u.evaluate(snap({ module: {} }), T0);
    expect(ids(u.evaluate(snap({ module: {} }), T0 + 5000))).toEqual([]);
    u.evaluate(snap({ module: null }), T0 + 10000);
    expect(ids(u.evaluate(snap({ module: null }), T0 + 15000))).toEqual([]);
  });

  it("keeps the first firing time while an alert keeps firing", () => {
    const t = new AlertTracker();
    t.evaluate(errors(0, 1), T0);
    const out = t.evaluate(errors(0, 2), T0 + 5000);
    expect(out[0].since).toBe(T0);
    expect(out[0].message).toContain("2 compressed origin responses");
  });

  it("acknowledgement hides an alert while its condition holds and re-arms after it clears", () => {
    const t = new AlertTracker();
    expect(ids(t.evaluate(errors(0, 2), T0))).toEqual(["origin-misconfiguration"]);
    t.acknowledge("origin-misconfiguration");
    expect(t.visible()).toEqual([]);
    expect(ids(t.evaluate(errors(0, 2), T0 + 5000))).toEqual([]);
    expect(ids(t.evaluate(errors(0, 0), T0 + 10000))).toEqual([]);
    expect(ids(t.evaluate(errors(0, 2), T0 + 15000))).toEqual(["origin-misconfiguration"]);
  });

  it("a firing state alert survives a sample where its check is undefined, without resetting its since", () => {
    const t = new AlertTracker();
    const first = t.evaluate(absent("unreachable"), T0);
    expect(ids(first)).toEqual(["daemon-unreachable"]);
    const since = first[0].since;
    // A fully transient sample: the rule's own check returns undefined.
    const blip = t.evaluate(absent("transient"), T0 + 5000);
    expect(ids(blip)).toEqual(["daemon-unreachable"]);
    expect(blip[0].since).toBe(since);
    const again = t.evaluate(absent("unreachable"), T0 + 10000);
    expect(ids(again)).toEqual(["daemon-unreachable"]);
    expect(again[0].since).toBe(since);
  });

  it("a firing alert survives a carried-forward sample", () => {
    // A stats-only 429 makes the overview re-use the previous statistics block
    // (toSnapshot's carry-forward): the same block evaluated again must
    // keep a firing, non-dismissed state alert up, and rate rules see no rise.
    const t = new AlertTracker();
    const first = errors(4, 2);
    t.evaluate(errors(1, 2), T0);
    expect(ids(t.evaluate(first, T0 + 5000))).toEqual(["error-rate", "origin-misconfiguration"]);
    const carried: AlertSnapshot = { ...first, module: { num_resource_fetch_failures: 0 } };
    expect(ids(t.evaluate(carried, T0 + 5000 + RATE_ALERT_HOLD_MS))).toEqual(["origin-misconfiguration"]);
  });

  it("transient samples raise nothing and keep acknowledgements", () => {
    const t = new AlertTracker();
    t.evaluate(errors(0, 2), T0);
    t.acknowledge("origin-misconfiguration");
    expect(t.evaluate(absent("transient"), T0 + 5000)).toEqual([]);
    expect(ids(t.evaluate(errors(0, 2), T0 + 10000))).toEqual([]);
  });

  it("an outage is not bridged by rate rules", () => {
    const t = new AlertTracker();
    t.evaluate(errors(1), T0);
    expect(ids(t.evaluate(absent("unreachable"), T0 + 5000))).toEqual(["daemon-unreachable"]);
    expect(ids(t.evaluate(errors(5), T0 + 10000))).toEqual([]);
    expect(ids(t.evaluate(errors(6), T0 + 15000))).toEqual(["error-rate"]);
  });

  it("resetBaseline makes the next sample a baseline", () => {
    const t = new AlertTracker();
    t.evaluate(errors(1), T0);
    t.resetBaseline();
    expect(ids(t.evaluate(errors(9), T0 + 5000))).toEqual([]);
  });
});

describe("failedChecks", () => {
  it("lists checks that report pass:false or false", () => {
    expect(failedChecks({ checks: { a: { pass: true }, b: { pass: false }, c: false } })).toEqual(["b", "c"]);
    expect(failedChecks({})).toEqual([]);
    expect(failedChecks(null)).toEqual([]);
  });
});

describe("rankAlerts", () => {
  const a = (id: string, severity: "error" | "warning"): ActiveAlert => ({ id, label: id, severity, message: id, fix: "", details: [], since: 0 });
  it("puts errors first and keeps rule order within a severity", () => {
    expect(ids(rankAlerts([a("w1", "warning"), a("e1", "error"), a("w2", "warning"), a("e2", "error")]))).toEqual([
      "e1",
      "e2",
      "w1",
      "w2",
    ]);
  });
});

describe("healthSummary", () => {
  const a = (severity: AlertSeverity): ActiveAlert => ({
    id: severity,
    label: "x",
    severity,
    message: "x",
    fix: "",
    details: [],
    since: 0,
  });
  const warnings = (count: number, withErrors = false): ActiveAlert => ({
    ...a("info"),
    id: "recent-warnings",
    count,
    countNoun: withErrors ? { one: "warning or error", many: "warnings and errors" } : { one: "warning", many: "warnings" },
  });
  it("says what needs attention, and stays calm when nothing does", () => {
    expect(healthSummary([], true)).toEqual({ text: "No findings", level: "healthy" });
    expect(healthSummary([], true, [a("warning"), a("error")])).toEqual({ text: "No findings · 2 acknowledged", level: "healthy" });
    expect(healthSummary([], false)).toEqual({ text: "Module statistics unavailable", level: "warning" });
    expect(healthSummary([a("warning")], true)).toEqual({ text: "1 finding · 1 needs action", level: "warning" });
    expect(healthSummary([a("warning"), a("error")], true)).toEqual({ text: "2 findings · 2 need action", level: "error" });
    expect(healthSummary([a("info")], true)).toEqual({ text: "1 finding", level: "info" });
    expect(healthSummary([a("error"), a("info"), a("info")], true, [a("warning")])).toEqual({
      text: "3 findings · 1 needs action · 1 acknowledged",
      level: "error",
    });
  });

  it("states the recent-warning count whenever that finding is present, listed or acknowledged", () => {
    expect(healthSummary([warnings(15)], true)).toEqual({ text: "1 finding · 15 warnings in the last 15 min", level: "info" });
    expect(healthSummary([a("error"), a("warning"), warnings(15)], true).text).toBe(
      "3 findings · 2 need action · 15 warnings in the last 15 min",
    );
    expect(healthSummary([], true, [warnings(15)])).toEqual({
      text: "No findings · 15 warnings in the last 15 min · 1 acknowledged",
      level: "healthy",
    });
    expect(healthSummary([a("warning")], true, [warnings(1)]).text).toBe(
      "1 finding · 1 needs action · 1 warning in the last 15 min · 1 acknowledged",
    );
  });

  it("says warnings and errors when the recent count includes errors", () => {
    expect(healthSummary([warnings(14, true)], true).text).toBe("1 finding · 14 warnings and errors in the last 15 min");
    expect(healthSummary([], true, [warnings(1, true)]).text).toBe(
      "No findings · 1 warning or error in the last 15 min · 1 acknowledged",
    );
  });

  it("never says 'No findings' when the message log could not be read", () => {
    expect(healthSummary([], true, [], true)).toEqual({
      text: "Findings unavailable — the message log could not be read",
      level: "info",
    });
    expect(healthSummary([], false, [], true)).toEqual({ text: "Module statistics unavailable", level: "warning" });
    // Findings that did fire are still counted.
    expect(healthSummary([a("warning")], true, [], true)).toEqual({ text: "1 finding · 1 needs action", level: "warning" });
  });
});

describe("optimizer error alert deep link", () => {
  it("the Optimizer errors rule links to the error-filtered optimizer Logs", () => {
    expect(ALERT_RULES.find((r) => r.id === "error-rate")?.link).toBe("#/logs?source=optimizer&level=error");
  });
});

// ── Findings (named rules) ─────────────────────────────────────────────

// A realistic epoch, so start times well before it stay positive.
const T1 = 1_790_000_000_000;

const ACL =
  "mod_pagespeed: admin handler 'pagespeed_global_admin' received request from N.N.N.N; intended for loopback. " +
  "Restrict access at the web-server layer (Apache: <Location> Require local; nginx: allow N.N.N.N; deny all; " +
  "IIS: InfoUrlsLocalOnly). See URL";
const VOLUME =
  "in-place optimization is OFF: the optimizer daemon publishes cache directory generation N (cache_dir_generation) " +
  "and this module is built for generation N. The two cache layouts share nothing; nothing was opened and nothing " +
  "was created. Install a module and daemon package pair that agree.";
const OPEN_FAILED =
  "nothing will be recorded for in-place optimization: cannot open the optimizer daemon's cache volume at " +
  "/var/cache/pagespeed-optimizer/vN: permission denied (attempt N of N)";

const group = (template: string, over: Partial<MessageGroup> = {}): MessageGroup => ({
  level: "warning",
  template,
  count: 1,
  recent: 0,
  lastMs: T1 - 60_000,
  ...over,
});
const withLog = (groups: MessageGroup[], over: Partial<AlertSnapshot> = {}): AlertSnapshot =>
  snap({ at: T1, messages: { groups, source: "module", truncated: false }, ...over });

describe("findings from the module's message log", () => {
  it("admin-exposed: the exposure warning anywhere in the retained log, with its fix and documentation", () => {
    const out = new AlertTracker().evaluate(withLog([group(ACL)]), T1);
    expect(ids(out)).toEqual(["admin-exposed"]);
    expect(out[0].severity).toBe("warning");
    expect(out[0].message).toBe(
      `An admin page was requested from an address other than this server itself (last at ${formatClock(T1 - 60_000)}).`,
    );
    expect(out[0].fix).toContain("Require local");
    expect(out[0].doc).toBe("https://modpagespeed.com/1.1/docs/admin-console/#access-control");
    expect(out[0].link).toBe("#/logs?source=module&level=warning");
    expect(out[0].details).toEqual([]);
  });

  // The module's warning, as it logs it, for a given handler and client address.
  const exposureLine = (handler: string, client: string): string =>
    `mod_pagespeed: admin handler '${handler}' received request from ${client}; intended for loopback. ` +
    "Restrict access at the web-server layer (Apache: <Location> Require local; nginx: allow 127.0.0.1; deny all; " +
    "IIS: InfoUrlsLocalOnly). See https://www.modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle";

  it("admin-exposed: an IPv6 client address and the other admin handler raise it too", () => {
    for (const line of [
      exposureLine("pagespeed_global_admin", "::1"),
      exposureLine("pagespeed_global_admin", "2001:db8::1"),
      exposureLine("pagespeed_admin", "172.18.0.4"),
      exposureLine("pagespeed_admin", "fe80::1ff:fe23:4567:890a"),
      exposureLine("(unknown)", "(unknown)"),
    ]) {
      const out = new AlertTracker().evaluate(withLog([group(messageTemplate(line))]), T1);
      expect(ids(out), line).toEqual(["admin-exposed"]);
    }
    const error = new AlertTracker().evaluate(withLog([group(ACL, { level: "error" })]), T1);
    expect(ids(error)).toEqual(["admin-exposed"]);
  });

  it("admin-exposed: only the module's own warning, never other text that holds both phrases", () => {
    const quiet = (g: MessageGroup): void => {
      expect(ids(new AlertTracker().evaluate(withLog([g]), T1)), g.template).toEqual([]);
    };
    // The same text at Info level is not the module's warning.
    quiet(group(ACL, { level: "info" }));
    // Both phrases, but not the module's fixed text.
    quiet(group("Fetch of URL failed: received request from N.N.N.N; intended for loopback."));
    quiet(group("upstream said: mod_pagespeed: admin handler 'x' received request from N; intended for loopback."));
    // The phrases out of order.
    quiet(group("mod_pagespeed: admin handler 'x' intended for loopback; received request from N.N.N.N"));
  });

  it("log rules have no data without a log read, and stay clear when the log lacks their templates", () => {
    expect(new AlertTracker().evaluate(snap({ at: T1 }), T1)).toEqual([]);
    expect(new AlertTracker().evaluate(snap({ at: T1, messages: null }), T1)).toEqual([]);
    expect(new AlertTracker().evaluate(withLog([group("Fetch of URL failed")]), T1)).toEqual([]);
  });

  it("a failed log read keeps a firing log finding and its acknowledgement; a clean log clears both", () => {
    const t = new AlertTracker();
    t.evaluate(withLog([group(ACL)]), T1);
    t.acknowledge("admin-exposed");
    t.evaluate(snap({ at: T1 + 5000, messages: null }), T1 + 5000);
    expect(t.visible()).toEqual([]);
    expect(ids(t.acknowledged())).toEqual(["admin-exposed"]);
    t.evaluate(withLog([]), T1 + 10_000);
    expect(t.acknowledged()).toEqual([]);
    expect(ids(t.evaluate(withLog([group(ACL)]), T1 + 15_000))).toEqual(["admin-exposed"]);
  });

  it("volume-not-opened: a recent cache-generation refusal is an error, ranks first, and names the two settings", () => {
    for (const level of ["warning", "error"] as const) {
      const out = new AlertTracker().evaluate(withLog([group(ACL), group(VOLUME, { level, recent: 1 })]), T1);
      expect(ids(out), level).toEqual(["volume-not-opened", "admin-exposed", "recent-warnings"]);
      expect(out[0].severity).toBe("error");
      expect(out[0].message).toContain(
        "The web server is not using the optimizer's cache: in-place optimization is OFF: the optimizer daemon " +
          "publishes cache directory generation N",
      );
      expect(out[0].fix).toBe("Make the web server's DaemonVolumePath and the optimizer's cache directory (--cache-dir) agree.");
      expect(out[0].doc).toBe("https://modpagespeed.com/1.1/docs/troubleshooting/#how-to-get-more-diagnostic-information");
    }
  });

  it("volume-not-opened: the start-up race, other refusals, an info line and an old refusal are not a trigger", () => {
    const notPublished =
      "in-place optimization is OFF: the optimizer daemon has not published the size of its cache volume at " +
      "/var/cache/pagespeed-optimizer/vN (it may not be running yet). This module will not open that volume without " +
      "knowing the size the daemon uses.";
    for (const g of [
      group(OPEN_FAILED, { level: "warning", recent: 1 }), // attempt 1 of 5: may heal on the next attempt
      group(OPEN_FAILED, { level: "error", recent: 1 }),
      group(notPublished, { level: "error", recent: 1 }),
      group(VOLUME, { level: "info", recent: 1 }),
      group(VOLUME, { level: "error", recent: 0, count: 3 }), // logged at start-up, more than 15 minutes ago
    ]) {
      const out = ids(new AlertTracker().evaluate(withLog([g]), T1));
      expect(out, `${g.level} recent=${g.recent} ${g.template.slice(0, 40)}`).not.toContain("volume-not-opened");
    }
  });

  it("volume-not-opened: the log half clears once the refusal is no longer recent", () => {
    const t = new AlertTracker();
    expect(ids(t.evaluate(withLog([group(VOLUME, { level: "warning", recent: 1 })]), T1))).toContain("volume-not-opened");
    const later = T1 + RATE_ALERT_HOLD_MS;
    expect(ids(t.evaluate(withLog([group(VOLUME, { level: "warning", recent: 0 })], { at: later }), later))).not.toContain(
      "volume-not-opened",
    );
  });

  it("a truncated log that lacks a template is no data, not a clear: the acknowledgement stays", () => {
    const t = new AlertTracker();
    t.evaluate(withLog([group(ACL)]), T1);
    t.acknowledge("admin-exposed");
    const truncated = snap({ at: T1 + 5000, messages: { groups: [group("other")], source: "module", truncated: true } });
    t.evaluate(truncated, T1 + 5000);
    expect(ids(t.acknowledged())).toEqual(["admin-exposed"]);
    t.evaluate(withLog([group("other")], { at: T1 + 10_000 }), T1 + 10_000);
    expect(t.acknowledged()).toEqual([]);
  });

  it("recent-warnings: one info finding with the count, the kinds and the three most frequent", () => {
    const out = new AlertTracker().evaluate(
      withLog([
        group("Slow origin response for URL", { recent: 12, count: 30 }),
        group("Fetch of URL failed", { level: "error", recent: 2, count: 3 }),
        group("CycloneCache enabled at /var/cache/mod_pagespeed/", { level: "info", recent: 1 }),
        group("old warning", { recent: 0, count: 9 }),
        group("x", { recent: 1 }),
        group("y", { recent: 1 }),
      ]),
      T1,
    );
    expect(ids(out)).toEqual(["recent-warnings"]);
    expect(out[0].severity).toBe("info");
    expect(out[0].message).toBe("16 warnings and errors in the last 15 minutes, of 4 kinds.");
    expect(out[0].count).toBe(16);
    expect(out[0].countNoun).toEqual({ one: "warning or error", many: "warnings and errors" });
    expect(out[0].details).toEqual(["×12 Slow origin response for URL", "×2 Fetch of URL failed", "×1 x"]);
    expect(out[0].link).toBe("#/logs?source=module&level=warning");
    const one = new AlertTracker().evaluate(withLog([group("slow N", { recent: 1 })]), T1);
    // Only warnings counted: the finding says "warning", as the summary line does.
    expect(one[0].message).toBe("1 warning in the last 15 minutes, of 1 kind.");
    expect(one[0].countNoun).toEqual({ one: "warning", many: "warnings" });
    const three = new AlertTracker().evaluate(withLog([group("slow N", { recent: 2 }), group("x", { recent: 1 })]), T1);
    expect(three[0].message).toBe("3 warnings in the last 15 minutes, of 2 kinds.");
    const oneError = new AlertTracker().evaluate(withLog([group("Fetch of URL failed", { level: "error", recent: 1 })]), T1);
    expect(oneError[0].message).toBe("1 warning or error in the last 15 minutes, of 1 kind.");
  });

  it("recent-warnings: details carry template text verbatim (rendered as text by the page)", () => {
    const out = new AlertTracker().evaluate(withLog([group("<img src=x onerror=alert(N)>", { recent: 1 })]), T1);
    expect(out[0].details).toEqual(["×1 <img src=x onerror=alert(N)>"]);
  });

  it("recent-warnings: a kind whose message is empty shows the shared empty-message text", () => {
    const out = new AlertTracker().evaluate(withLog([group("", { recent: 2 }), group("   ", { recent: 1 })]), T1);
    expect(out[0].details).toEqual([`×2 ${EMPTY_MESSAGE_TEXT}`, `×1 ${EMPTY_MESSAGE_TEXT}`]);
  });
});

describe("findings from the optimizer's time base and the build", () => {
  const started = (agoMs: number): AlertSnapshot =>
    snap({ at: T1, daemonStats: { ...(snap().daemonStats as DaemonStatsResponse), started_at_ms: T1 - agoMs } });

  it("optimizer-restarted: started less than 15 minutes ago", () => {
    const out = new AlertTracker().evaluate(started(5 * 60_000), T1);
    expect(ids(out)).toEqual(["optimizer-restarted"]);
    expect(out[0].severity).toBe("info");
    expect(out[0].message).toBe("The optimizer started 5 min ago; its statistics cover only the time since then.");
    expect(new AlertTracker().evaluate(started(30_000), T1)[0].message).toBe(
      "The optimizer started less than a minute ago; its statistics cover only the time since then.",
    );
    expect(new AlertTracker().evaluate(started(WARMUP_MS), T1)).toEqual([]);
  });

  it("optimizer-restarted: an optimizer without started_at_ms is timed by its health uptime", () => {
    const byUptime = snap({ at: T1, health: { ...(snap().health as DaemonHealthResponse), uptime_seconds: 120 } });
    expect(new AlertTracker().evaluate(byUptime, T1)[0].message).toBe(
      "The optimizer started 2 min ago; its statistics cover only the time since then.",
    );
  });

  it("optimizer-restarted: a start up to 5 minutes ahead of this clock is skew, a just-started optimizer", () => {
    expect(new AlertTracker().evaluate(started(-4 * 60_000), T1)[0].message).toBe(
      "The optimizer started less than a minute ago; its statistics cover only the time since then.",
    );
    expect(ids(new AlertTracker().evaluate(started(-5 * 60_000), T1))).toEqual(["optimizer-restarted"]);
    expect(new AlertTracker().evaluate(started(-5 * 60_000 - 1), T1)).toEqual([]);
  });

  it("optimizer-restarted: no start time, a start far in the future, or no optimizer is no finding", () => {
    expect(new AlertTracker().evaluate(snap({ at: T1 }), T1)).toEqual([]);
    expect(new AlertTracker().evaluate(started(-10 * 60_000), T1)).toEqual([]);
    expect(ids(new AlertTracker().evaluate({ ...absent("unreachable"), at: T1 }, T1))).toEqual(["daemon-unreachable"]);
  });

  it("daemon-recovered: an outage, the first load included, is remembered for 15 minutes once the optimizer answers", () => {
    const t = new AlertTracker();
    expect(ids(t.evaluate({ ...absent("unreachable"), at: T1, daemonDownAt: T1 }, T1))).toEqual(["daemon-unreachable"]);
    const back = t.evaluate(snap({ at: T1 + 5000, daemonDownAt: T1 }), T1 + 5000);
    expect(ids(back)).toEqual(["daemon-recovered"]);
    expect(back[0].severity).toBe("info");
    expect(back[0].message).toBe(`The optimizer did not answer at ${formatClock(T1)}; it is answering again.`);
    expect(t.evaluate(snap({ at: T1 + WARMUP_MS, daemonDownAt: T1 }), T1 + WARMUP_MS)).toEqual([]);
    expect(new AlertTracker().evaluate(snap({ at: T1, daemonDownAt: null }), T1)).toEqual([]);
  });

  it("dirty-build: a build stamp with uncommitted changes is an info finding", () => {
    const out = new AlertTracker().evaluate(snap({ buildStamp: "v1.16.0-86-g0cdb738df-dirty" }), T0);
    expect(ids(out)).toEqual(["dirty-build"]);
    expect(out[0].severity).toBe("info");
    expect(out[0].message).toContain("(v1.16.0-86-g0cdb738df-dirty)");
    expect(out[0].link).toBe("#/about");
    expect(new AlertTracker().evaluate(snap({ buildStamp: "v1.16.0" }), T0)).toEqual([]);
    expect(new AlertTracker().evaluate(snap({ buildStamp: "dev" }), T0)).toEqual([]);
  });
});

describe("volume-not-opened from the counters", () => {
  const vars = (served: number, css: number, js: number, image: number): Record<string, number> => ({
    num_resource_fetch_failures: 0,
    ipro_daemon_served: served,
    ipro_daemon_fallthrough: css + js + image + 1000,
    ipro_daemon_fallthrough_css: css,
    ipro_daemon_fallthrough_js: js,
    ipro_daemon_fallthrough_image: image,
  });
  /** How long before T1 each side started; `module: null` = a module that does not report process_start_ms. */
  interface Ages {
    optimizer?: number;
    module?: number | null;
  }
  const sample = (module: Record<string, number>, at: number, ages: Ages = {}): AlertSnapshot => {
    const moduleAgo = ages.module === undefined ? 3 * WARMUP_MS : ages.module;
    return snap({
      at,
      module: moduleAgo === null ? module : { ...module, process_start_ms: T1 - moduleAgo },
      messages: { groups: [], source: "module", truncated: false },
      daemonStats: { ...(snap().daemonStats as DaemonStatsResponse), started_at_ms: T1 - (ages.optimizer ?? 3 * WARMUP_MS) },
    });
  };
  /** Evaluate the samples in order on one tracker; the ids after the last one. */
  const run = (steps: Array<[Record<string, number>, number]>, ages: Ages = {}): string[] => {
    const t = new AlertTracker();
    let out: ActiveAlert[] = [];
    for (const [v, at] of steps) out = t.evaluate(sample(v, at, ages), at);
    return ids(out);
  };
  const W = VOLUME_WINDOW_MS;

  /** A tracker whose only rule records the earlier samples it is handed. */
  const recording = () => {
    const seen: { history: readonly unknown[] } = { history: [] };
    const probe: AlertRule = {
      id: "probe",
      label: "probe",
      severity: "info",
      kind: "state",
      check: (_cur, _prev, history) => {
        seen.history = [...history];
        return undefined;
      },
      message: () => "",
      fix: "",
    };
    return { tracker: new AlertTracker([probe]), seen };
  };

  it("keeps only the time and the counters the window reads from each earlier sample", () => {
    const { tracker, seen } = recording();
    const bigLog: MessageGroup[] = Array.from({ length: 500 }, (_, i) => group(`warning number ${"x".repeat(i)}`));
    for (let i = 0; i < 3; i++) {
      const at = T1 + i * 5_000;
      tracker.evaluate({ ...sample(vars(100 + i, 10, 10, 10), at), messages: { groups: bigLog, source: "module", truncated: false } }, at);
    }
    tracker.evaluate(sample(vars(103, 10, 10, 10), T1 + 15_000), T1 + 15_000);
    expect(seen.history).toHaveLength(3);
    for (const [i, kept] of seen.history.entries()) {
      expect(Object.keys(kept as object).sort()).toEqual(["at_ms", "counters"]);
      expect(kept).toEqual({
        at_ms: T1 + i * 5_000,
        counters: {
          ipro_daemon_served: 100 + i,
          ipro_daemon_fallthrough_css: 10,
          ipro_daemon_fallthrough_js: 10,
          ipro_daemon_fallthrough_image: 10,
        },
      });
    }
  });

  it("a counter the module does not report is left out of the kept sample, never kept as a gap", () => {
    const { tracker, seen } = recording();
    const partial = { ipro_daemon_served: 5, ipro_daemon_fallthrough_css: Number.NaN } as Record<string, number>;
    tracker.evaluate(sample(partial, T1), T1);
    tracker.evaluate(sample(partial, T1 + 5_000), T1 + 5_000);
    expect(seen.history).toEqual([{ at_ms: T1, counters: { ipro_daemon_served: 5 } }]);
  });

  it("the kept samples stay bounded: the window at a 5 s refresh, and the length cap at a fast one", () => {
    const { tracker, seen } = recording();
    for (let i = 0; i <= 2 * 181; i++) tracker.evaluate(sample(vars(i, 0, 0, 0), T1 + i * 5_000), T1 + i * 5_000);
    // The samples since the newest one at least the window old, before the current one.
    expect(seen.history.length).toBe(181);
    const fast = recording();
    const n = MAX_HISTORY + 500;
    for (let i = 0; i < n; i++) fast.tracker.evaluate(sample(vars(i, 0, 0, 0), T1 + i * 100), T1 + i * 100);
    expect(fast.seen.history.length).toBe(MAX_HISTORY);
    expect(MAX_HISTORY).toBe(1024);
  });

  it("fires when at least 50 optimizable requests fell through over the last 15 minutes with nothing served", () => {
    const t = new AlertTracker();
    t.evaluate(sample(vars(100, 10, 10, 10), T1 - W), T1 - W);
    t.evaluate(sample(vars(100, 20, 20, 15), T1 - W / 2), T1 - W / 2);
    const out = t.evaluate(sample(vars(100, 30, 30, 20), T1), T1);
    expect(ids(out)).toEqual(["volume-not-opened"]);
    expect(out[0].message).toBe(
      "The optimizer is answering, but none of the last 50 CSS, JavaScript and image requests was served from its cache.",
    );
  });

  it("a window shorter than 15 minutes is no evidence, however many requests fell through", () => {
    expect(W).toBe(15 * 60_000);
    expect(run([[vars(100, 10, 10, 10), T1 - 5000], [vars(100, 500, 500, 500), T1]])).toEqual([]);
    expect(run([[vars(100, 10, 10, 10), T1 - W + 1000], [vars(100, 500, 500, 500), T1]])).toEqual([]);
  });

  it("only the last 15 minutes count: requests before the window do not", () => {
    expect(
      run([
        [vars(100, 0, 0, 0), T1 - 2 * W],
        [vars(100, 40, 40, 40), T1 - W],
        [vars(100, 50, 50, 49), T1],
      ]),
    ).toEqual([]);
  });

  it("stays quiet when something was served within the window, or fewer than 50 requests fell through", () => {
    expect(
      run([
        [vars(100, 10, 10, 10), T1 - W],
        [vars(101, 20, 20, 10), T1 - W / 2],
        [vars(101, 30, 30, 20), T1],
      ]),
    ).toEqual([]);
    expect(run([[vars(100, 10, 10, 10), T1 - W], [vars(100, 30, 30, 19), T1]])).toEqual([]);
  });

  it("stays quiet while either side is warming up: a fresh optimizer, or a freshly started web server", () => {
    const steps: Array<[Record<string, number>, number]> = [
      [vars(100, 10, 10, 10), T1 - W],
      [vars(100, 40, 40, 40), T1],
    ];
    expect(run(steps)).toEqual(["volume-not-opened"]);
    expect(run(steps, { optimizer: 60_000 })).toEqual(["optimizer-restarted"]);
    expect(run(steps, { module: 60_000 })).toEqual([]);
    expect(run(steps, { module: WARMUP_MS - 1 })).toEqual([]);
  });

  it("stays quiet without the module's start time or a per-class counter (an older module)", () => {
    const steps = (a: Record<string, number>, b: Record<string, number>): Array<[Record<string, number>, number]> => [
      [a, T1 - W],
      [b, T1],
    ];
    expect(run(steps(vars(100, 10, 10, 10), vars(100, 40, 40, 40)), { module: null })).toEqual([]);
    const older = (v: Record<string, number>) => {
      const { ipro_daemon_fallthrough_js: _js, ...rest } = v;
      return rest;
    };
    expect(run(steps(older(vars(100, 10, 10, 10)), older(vars(100, 40, 40, 40))))).toEqual([]);
  });

  it("a page view starts a fresh window: nothing sampled before resetBaseline counts", () => {
    const t = new AlertTracker();
    t.evaluate(sample(vars(100, 10, 10, 10), T1 - W), T1 - W);
    t.resetBaseline();
    expect(ids(t.evaluate(sample(vars(100, 40, 40, 40), T1), T1))).toEqual([]);
  });
});

describe("ranking and acknowledgement", () => {
  const a = (id: string, severity: AlertSeverity): ActiveAlert => ({
    id,
    label: id,
    severity,
    message: id,
    fix: "",
    details: [],
    since: 0,
  });

  it("ranks errors, then warnings, then info", () => {
    expect(ids(rankAlerts([a("i1", "info"), a("w1", "warning"), a("e1", "error"), a("i2", "info")]))).toEqual([
      "e1",
      "w1",
      "i1",
      "i2",
    ]);
  });

  it("an acknowledged finding leaves the list for the acknowledged group, and comes back when un-acknowledged", () => {
    const t = new AlertTracker();
    t.evaluate(errors(0, 2), T0);
    t.acknowledge("origin-misconfiguration");
    expect(t.visible()).toEqual([]);
    expect(ids(t.acknowledged())).toEqual(["origin-misconfiguration"]);
    expect(t.acknowledged()[0].fix).toContain("uncompressed responses");
    t.unacknowledge("origin-misconfiguration");
    expect(ids(t.visible())).toEqual(["origin-misconfiguration"]);
    expect(t.acknowledged()).toEqual([]);
  });

  it("a cleared finding forgets its acknowledgement, so it comes back armed", () => {
    const acks = memoryAckStore();
    const t = new AlertTracker(ALERT_RULES, RATE_ALERT_HOLD_MS, acks);
    t.evaluate(errors(0, 2), T0);
    t.acknowledge("origin-misconfiguration");
    expect(acks.has("origin-misconfiguration")).toBe(true);
    t.evaluate(errors(0, 0), T0 + 5000);
    expect(acks.has("origin-misconfiguration")).toBe(false);
    expect(ids(t.evaluate(errors(0, 2), T0 + 10_000))).toEqual(["origin-misconfiguration"]);
  });

  it("an acknowledgement survives a reload through the store", () => {
    const m = new Map<string, string>();
    const storage = {
      getItem: (k: string) => m.get(k) ?? null,
      setItem: (k: string, v: string) => void m.set(k, v),
    } as unknown as Storage;
    const first = new AlertTracker(ALERT_RULES, RATE_ALERT_HOLD_MS, createAckStore(storage));
    first.evaluate(errors(0, 2), T0);
    first.acknowledge("origin-misconfiguration");
    const second = new AlertTracker(ALERT_RULES, RATE_ALERT_HOLD_MS, createAckStore(storage));
    expect(second.evaluate(errors(0, 2), T0 + 5000)).toEqual([]);
    expect(ids(second.acknowledged())).toEqual(["origin-misconfiguration"]);
  });
});

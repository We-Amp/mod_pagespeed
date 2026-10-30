// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The honest-numbers set, against fixtures rebuilt from measured times and totals from a live console
// (test-fixtures/README-honest-numbers.md says where each came from).

import { describe, expect, it } from "vitest";
import { AlertTracker, healthSummary, type AlertSnapshot } from "$lib/alerts";
import idleVhost from "../../../test-fixtures/stats-json-idle-vhost.fixture.json";
import youngBuffer from "../../../test-fixtures/message-history-young-buffer.fixture.json";
import { formatPercent } from "./format";
import { lastSampleForDisplay } from "./graph-series";
import { toMessageDigest } from "./message-groups";
import { optimizedCopyHitRate } from "./savings";
import { SAVINGS_QUALIFIER } from "./serve-savings";

describe("0 of 0 reads as a dash, never 0%", () => {
  it("an idle vhost's hit rate has no percentage", () => {
    const rate = optimizedCopyHitRate(idleVhost.variables);
    expect(rate.total).toBe(0);
    expect(rate.percent).toBeNull();
    expect(formatPercent(rate.percent, rate.served)).toBe("—");
  });

  it("a share chart whose newest interval had no requests shows no latest value", () => {
    expect(lastSampleForDisplay([40, 25, null])).toBeNull();
    expect(lastSampleForDisplay([40, 25, 0])).toBe(0);
    expect(lastSampleForDisplay([])).toBeNull();
  });
});

describe("a young message buffer states the window it covers", () => {
  const now = youngBuffer.now_ms;
  const snapshot = (): AlertSnapshot => ({
    module: {},
    daemon: "not-configured",
    health: null,
    daemonStats: null,
    daemonStatsUnsupported: false,
    belowFloor: false,
    at: now,
    messages: toMessageDigest(youngBuffer, now),
  });

  it("reads first_ms from the module's answer", () => {
    expect(toMessageDigest(youngBuffer, now)?.firstMs).toBe(youngBuffer.first_ms);
  });

  it("a 4-minute buffer against the 15-minute claim prints the true window", () => {
    const tracker = new AlertTracker();
    const out = tracker.evaluate(snapshot(), now);
    const warnings = out.find((a) => a.id === "recent-warnings");
    expect(warnings).toBeDefined();
    expect(warnings?.message).toContain("in the retained messages since ");
    expect(warnings?.message).not.toContain("last 15 minutes");
    expect(warnings?.countWindow).toMatch(/^in the retained messages since \d\d:\d\d$/);
    const summary = healthSummary(out, true);
    expect(summary.text).toContain("in the retained messages since ");
    expect(summary.text).not.toContain("last 15 min");
  });

  it("keeps the 15-minute claim once the buffer reaches back that far", () => {
    const old = { ...youngBuffer, first_ms: now - 20 * 60_000 };
    const snap: AlertSnapshot = { ...snapshot(), messages: toMessageDigest(old, now) };
    const out = new AlertTracker().evaluate(snap, now);
    expect(out.find((a) => a.id === "recent-warnings")?.message).toContain("in the last 15 minutes");
    expect(healthSummary(out, true).text).toContain("in the last 15 min");
  });
});

describe("the savings qualifier", () => {
  it("names the unit and that compression is included", () => {
    expect(SAVINGS_QUALIFIER.toLowerCase()).toContain("measured in transfer bytes, includes compression");
  });
});

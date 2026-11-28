// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { STAT_DESCRIPTIONS, statDescriptionSource } from "../data/stat-descriptions";
import {
  HTML_CACHE_WARNINGS,
  HTML_CACHE_TEXT,
  CONFIG_RETRY_FIRST_MS,
  CONFIG_RETRY_MAX_MS,
  configRetryDelayMs,
  htmlCacheMode,
  htmlCacheView,
} from "./html-cache";

describe("configRetryDelayMs", () => {
  it("doubles the wait for each failure in a row, up to the cap", () => {
    expect(configRetryDelayMs(1)).toBe(CONFIG_RETRY_FIRST_MS);
    expect(configRetryDelayMs(2)).toBe(2 * CONFIG_RETRY_FIRST_MS);
    expect(configRetryDelayMs(3)).toBe(4 * CONFIG_RETRY_FIRST_MS);
    expect(configRetryDelayMs(10)).toBe(CONFIG_RETRY_MAX_MS);
    expect(configRetryDelayMs(10_000)).toBe(CONFIG_RETRY_MAX_MS);
  });
});

describe("htmlCacheView", () => {
  it("is off when no counter moved", () => {
    const view = htmlCacheView({});
    expect(view.state).toBe("off");
    expect(view.wouldStorePercent).toBeNull();
    expect(view.reasons).toEqual([]);
  });

  it("says unavailable when only the unavailable counter moved", () => {
    expect(htmlCacheView({ daemon_html_unavailable: 3 }).state).toBe("unavailable");
  });

  it("computes the share and orders the reasons", () => {
    const view = htmlCacheView({
      daemon_html_evaluated: 8,
      daemon_html_would_store: 2,
      daemon_html_refused: 6,
      daemon_html_refused_vary: 1,
      daemon_html_refused_no_lifetime: 4,
      daemon_html_refused_cookie_request: 1,
      daemon_html_refused_url: 0,
    });
    expect(view.state).toBe("reporting");
    expect(view.wouldStorePercent).toBe(25);
    expect(view.reasons).toEqual([
      { name: "no_lifetime", count: 4 },
      { name: "cookie_request", count: 1 },
      { name: "vary", count: 1 },
    ]);
  });

  it("takes the state from the configured mode when it is known", () => {
    expect(htmlCacheView({}, "report-only").state).toBe("waiting");
    expect(htmlCacheView({ daemon_html_evaluated: 4 }, "off").state).toBe("off");
    expect(htmlCacheView({ daemon_html_unavailable: 2 }, "report-only").state).toBe("unavailable");
    const view = htmlCacheView({ daemon_html_evaluated: 4, daemon_html_unavailable: 2 }, "report-only");
    expect(view.state).toBe("reporting");
    expect(view.unavailable).toBe(2);
  });

  it("reads the mode from the configuration text", () => {
    const listed = "Version: 14: on\n\nOptions\n  DaemonHtmlCache (dhc)   report-only\n  LogDir (ld)  /tmp\n";
    expect(htmlCacheMode({ config: "", effective_config: listed })).toBe("report-only");
    expect(htmlCacheMode({ config: listed })).toBe("report-only");
    expect(htmlCacheMode({ config: "Version: 14: on\n\nOptions\n  DaemonHtmlCache (dhc)  off\n" })).toBe("off");
    // Not listed: the option is at its default.
    expect(htmlCacheMode({ config: "Version: 14: on\n\nOptions\n" })).toBe("off");
    expect(htmlCacheMode(null)).toBeNull();
    expect(htmlCacheMode({ status: "busy" })).toBeNull();
  });

  it("treats malformed counters as zero", () => {
    const view = htmlCacheView({ daemon_html_evaluated: "x", daemon_html_refused_vary: -2 });
    expect(view.state).toBe("off");
    expect(view.reasons).toEqual([]);
  });
});

describe("serve-variants", () => {
  it("reads serve-variants from the configuration text", () => {
    const listed = "Version: 14: on\n\nOptions\n  DaemonHtmlCache (dhc)   serve-variants\n";
    expect(htmlCacheMode({ config: listed })).toBe("serve-variants");
    expect(htmlCacheView({}, "serve-variants").state).toBe("waiting");
    expect(htmlCacheView({}, "serve-variants").mode).toBe("serve-variants");
  });

  it("splits the Markdown 200s by coding and counts the compressed copies not sent", () => {
    const view = htmlCacheView(
      {
        daemon_markdown_served: 6,
        daemon_markdown_served_br: 3,
        daemon_markdown_served_gzip: 2,
        daemon_markdown_served_identity: 1,
        daemon_markdown_encoded_copy_missing: 4,
        daemon_markdown_encoded_copy_unbound: 1,
        daemon_markdown_compress_demand_sent: 2,
        daemon_markdown_coded_fallback: 3,
        daemon_markdown_fallthrough_no_acceptable_encoding: 5,
      },
      "serve-variants",
    );
    expect(view.markdownServedBr).toBe(3);
    expect(view.markdownServedGzip).toBe(2);
    expect(view.markdownServedIdentity).toBe(1);
    expect(view.encodedCopyMissing).toBe(4);
    expect(view.encodedCopyUnbound).toBe(1);
    expect(view.compressDemandSent).toBe(2);
    expect(view.codedFallback).toBe(3);
    expect(view.fallthroughs).toEqual([{ name: "no_acceptable_encoding", count: 5 }]);
    // An older library counts none of them: all read as zero.
    const old = htmlCacheView({ daemon_markdown_served: 6 }, "serve-variants");
    expect(old.markdownServedBr + old.markdownServedGzip + old.markdownServedIdentity).toBe(0);
  });

  it("counts stored, served and fall-through, largest first", () => {
    const view = htmlCacheView(
      {
        daemon_html_stored: 5,
        daemon_html_removed: 2,
        daemon_html_removed_on_refusal: 1,
        daemon_markdown_served: 7,
        daemon_markdown_not_modified: 3,
        daemon_markdown_fallthrough_absent: 2,
        daemon_markdown_fallthrough_stale: 9,
        daemon_html_refused_budget: 1,
        daemon_html_refused_vary: 4,
        daemon_html_refused_url_disagree: 2,
        daemon_html_refused_request_header: 3,
      },
      "serve-variants",
    );
    expect(view.state).toBe("reporting");
    expect(view.stored).toBe(5);
    expect(view.markdownServed).toBe(7);
    expect(view.fallthroughs).toEqual([
      { name: "stale", count: 9 },
      { name: "absent", count: 2 },
    ]);
    expect(view.storeRefusals).toEqual([
      { name: "request_header", count: 3 },
      { name: "url_disagree", count: 2 },
      { name: "budget", count: 1 },
    ]);
    // The engine's refusals are not report reasons.
    expect(view.reasons).toEqual([{ name: "vary", count: 4 }]);
    expect(view.servingBlocked).toBe(false);
  });

  it("says serving is blocked now only while the counter rises between polls", () => {
    const stats = { daemon_html_serve_unavailable: 4, daemon_html_evaluated: 4 };
    // First poll: no earlier sample, so it is history.
    const first = htmlCacheView(stats, "serve-variants", null);
    expect(first.servingBlocked).toBe(false);
    expect(first.servingUnavailableTotal).toBe(4);
    // Rising: blocked now.
    expect(htmlCacheView({ ...stats, daemon_html_serve_unavailable: 6 }, "serve-variants", 4).servingBlocked).toBe(true);
    // Flat after the cause was fixed: history only, with the count.
    const flat = htmlCacheView(stats, "serve-variants", 4);
    expect(flat.servingBlocked).toBe(false);
    expect(flat.servingUnavailableTotal).toBe(4);
    expect(htmlCacheView(stats, "report-only", 1).servingBlocked).toBe(false);
    expect(htmlCacheView(stats, "report-only", 1).servingUnavailableTotal).toBe(0);
  });

  it("names the causes, including the optimizer's cache and the module's AgentOptimize", () => {
    expect(HTML_CACHE_TEXT.degradedCauses).toEqual([
      "optimizer library below 1.14",
      "worker not in a markdown mode",
      "worker agent-optimize off",
      "the optimizer's shared cache is not open",
    ]);
    expect(HTML_CACHE_TEXT.agentOptimizeOff).toContain("AgentOptimize off");
    // The intro does not claim storing or serving in every state.
    expect(HTML_CACHE_TEXT.introServe).toContain("where this server");
    expect(HTML_CACHE_TEXT.introServe).toContain("otherwise the numbers are the report");
  });

  it("marks an inferred mode and treats an unknown listed value as unknown", () => {
    expect(htmlCacheView({ daemon_html_stored: 1 }).modeInferred).toBe(true);
    expect(htmlCacheView({}, "serve-variants").modeInferred).toBe(false);
    expect(htmlCacheView({}).modeInferred).toBe(false);
    expect(htmlCacheMode({ config: "Options\n  DaemonHtmlCache (dhc)   serve-cached-html\n" })).toBeNull();
  });

  it("shows exactly the five warning counters, only when they moved and the cache is on", () => {
    expect(HTML_CACHE_WARNINGS.map(([n]) => n)).toEqual([
      "daemon_html_sidecar_unpaired_stale",
      "daemon_html_epoch_write_failed",
      "daemon_html_purge_remove_failed",
      "daemon_html_refused_url_disagree",
      "daemon_markdown_transport_conflict",
    ]);
    const stats = { daemon_html_epoch_write_failed: 2, daemon_markdown_transport_conflict: 1 };
    expect(htmlCacheView(stats, "serve-variants").warnings.map((w) => w.name)).toEqual([
      "daemon_html_epoch_write_failed",
      "daemon_markdown_transport_conflict",
    ]);
    expect(htmlCacheView(stats, "off").warnings).toEqual([]);
    expect(htmlCacheView({}, "serve-variants").warnings).toEqual([]);
  });

  it("infers serve-variants from engine counters when the mode is unreadable", () => {
    expect(htmlCacheView({ daemon_html_stored: 1 }).mode).toBe("serve-variants");
    expect(htmlCacheView({}).mode).toBeNull();
  });

  it("describes every counter the module registers for the engine", () => {
    const engine = [
      "stored", "refused_budget", "refused_url_disagree", "refused_request_header", "sidecar_write_failed", "sidecar_unpaired_stale",
      "purge_race_refused", "removed", "removed_on_refusal", "restore_skipped", "epoch_write_failed",
      "purge_remove_failed", "serve_unavailable", "notify_sent", "notify_limited",
    ].map((n) => `daemon_html_${n}`);
    const markdown = [
      "served", "not_modified", "served_without_preference", "transport_conflict", "served_br", "served_gzip",
      "served_identity", "encoded_copy_missing", "encoded_copy_unbound", "compress_demand_sent", "coded_fallback",
    ].map(
      (n) => `daemon_markdown_${n}`,
    );
    const reasons = [
      "fresh", "mode", "auth_request", "cookie_request", "client_no_cache", "needs_validation", "stale",
      "headers_not_reproducible", "sidecar_missing", "purged", "absent", "binding_stale", "parent_stale",
      "method", "range", "cors", "no_transform", "withheld", "skew", "no_acceptable_encoding", "sidecar_unpaired", "url_disagree",
      "request_header", "other",
    ].map((n) => `daemon_markdown_fallthrough_${n}`);
    for (const name of [...engine, ...markdown, ...reasons]) {
      expect(statDescriptionSource(name), name).toBe("exact");
      expect(STAT_DESCRIPTIONS[name]!.length, name).toBeGreaterThan(10);
    }
  });
});

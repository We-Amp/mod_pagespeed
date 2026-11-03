// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
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

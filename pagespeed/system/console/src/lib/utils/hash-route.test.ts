// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { DEFAULT_PATH, LatestGate, isKnownSeverity, pageTitle, parseHash, severityFilter } from "./hash-route";

describe("parseHash", () => {
  it("splits the page path from its query", () => {
    const r = parseHash("#/messages?level=error");
    expect(r.path).toBe("#/messages");
    expect(r.params.get("level")).toBe("error");
  });
  it("a hash without a query has empty params", () => {
    const r = parseHash("#/statistics");
    expect(r.path).toBe("#/statistics");
    expect([...r.params.keys()]).toEqual([]);
  });
  it("an empty hash is the default page", () => {
    expect(parseHash("").path).toBe(DEFAULT_PATH);
    expect(parseHash("#").path).toBe(DEFAULT_PATH);
  });
});

describe("severityFilter", () => {
  it("a level shows that severity and everything more severe", () => {
    expect(severityFilter("error")).toEqual({ fatal: true, error: true, warning: false, info: false });
    expect(severityFilter("warning")).toEqual({ fatal: true, error: true, warning: true, info: false });
    expect(severityFilter("fatal")).toEqual({ fatal: true, error: false, warning: false, info: false });
  });
  it("no level, info, or an unknown level shows everything", () => {
    const all = { fatal: true, error: true, warning: true, info: true };
    expect(severityFilter(null)).toEqual(all);
    expect(severityFilter("info")).toEqual(all);
    expect(severityFilter("loud")).toEqual(all);
    expect(severityFilter("toString")).toEqual(all);
  });
});

describe("isKnownSeverity", () => {
  it("recognises the four severities", () => {
    expect(isKnownSeverity("fatal")).toBe(true);
    expect(isKnownSeverity("error")).toBe(true);
    expect(isKnownSeverity("warning")).toBe(true);
    expect(isKnownSeverity("info")).toBe(true);
  });
  it("an unknown level, an inherited property name, or no level at all is not known", () => {
    expect(isKnownSeverity("loud")).toBe(false);
    expect(isKnownSeverity("toString")).toBe(false);
    expect(isKnownSeverity(null)).toBe(false);
  });
});

describe("pageTitle", () => {
  it("names the page before the console", () => {
    expect(pageTitle("Messages", "mod_pagespeed Admin Console")).toBe("Messages — mod_pagespeed Admin Console");
  });
});

describe("LatestGate", () => {
  it("only the most recently started load is current", () => {
    const gate = new LatestGate();
    const first = gate.begin();
    const second = gate.begin();
    expect(gate.isCurrent(first)).toBe(false);
    expect(gate.isCurrent(second)).toBe(true);
  });
});

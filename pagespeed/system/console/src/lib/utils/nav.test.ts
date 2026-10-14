// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { SERVER_PAGE_LINKS, routes, type NavGroup } from "$lib/routes";
import { isNavActive, navKeyHint, serverPageHref } from "./nav";

const labels = (group: NavGroup, isGlobal: boolean) =>
  routes.filter((r) => r.group === group && r.hidden !== true && (r.globalOnly !== true || isGlobal)).map((r) => r.label);

describe("the sidebar", () => {
  it("lists the primary pages by group, plus Support and About", () => {
    expect(labels("overview", true)).toEqual(["Overview", "Savings", "URLs"]);
    expect(labels("module", true)).toEqual(["Statistics", "Histograms", "Caches", "Configuration", "Rule packs"]);
    expect(labels("optimizer", true)).toEqual(["Status", "Logs"]);
    expect(labels("optimizer", false)).toEqual(["Status", "Logs"]);
    expect(labels("help", true)).toEqual(["Support", "About"]);
    expect(labels("overview", false)).toEqual(["Overview", "Savings"]);
  });

  it("every hidden page names a visible parent", () => {
    for (const r of routes.filter((x) => x.hidden === true)) {
      expect(r.parent, r.path).toBeDefined();
      expect(routes.some((p) => p.path === r.parent && p.hidden !== true), r.path).toBe(true);
    }
  });
});

describe("server-rendered page links", () => {
  it("Rule packs is a native console page after Configuration, no longer a link out", () => {
    expect(SERVER_PAGE_LINKS).toEqual([]);
    const module = routes.filter((r) => r.group === "module" && r.hidden !== true).map((r) => r.label);
    expect(module).toEqual(["Statistics", "Histograms", "Caches", "Configuration", "Rule packs"]);
  });

  it("no link shadows a route's label, so each sidebar name stays unique", () => {
    for (const l of SERVER_PAGE_LINKS) expect(routes.some((r) => r.label === l.label), l.label).toBe(false);
  });

  it("points at the open console's own admin path", () => {
    expect(serverPageHref("/pagespeed_admin", "rule_packs")).toBe("/pagespeed_admin/rule_packs");
    expect(serverPageHref("/pagespeed_global_admin", "rule_packs")).toBe("/pagespeed_global_admin/rule_packs");
    expect(serverPageHref("/secret-admin/", "rule_packs")).toBe("/secret-admin/rule_packs");
    expect(serverPageHref("", "rule_packs")).toBe("/rule_packs");
  });
});

describe("navKeyHint", () => {
  it("names the shortcut of each page that has one", () => {
    expect(navKeyHint("#/statistics")).toBe("g s");
    expect(navKeyHint("#/optimizer")).toBe("g d");
    expect(navKeyHint("#/urls")).toBe("g u");
    expect(navKeyHint("#/overview")).toBe("g o");
  });

  it("a page without a shortcut, or an unknown path, has none", () => {
    expect(navKeyHint("#/support")).toBeNull();
    expect(navKeyHint("#/nope")).toBeNull();
  });
});

describe("isNavActive", () => {
  const byPath = (p: string) => routes.find((r) => r.path === p)!;

  it("a page is active on its own route only", () => {
    expect(isNavActive(byPath("#/statistics"), "#/statistics", routes)).toBe(true);
    expect(isNavActive(byPath("#/histograms"), "#/statistics", routes)).toBe(false);
  });

  it("a hidden page marks its parent: Graphs marks Statistics", () => {
    expect(isNavActive(byPath("#/statistics"), "#/graphs", routes)).toBe(true);
    expect(isNavActive(byPath("#/histograms"), "#/graphs", routes)).toBe(false);
  });

  it("an unknown path marks nothing", () => {
    expect(routes.some((r) => isNavActive(r, "#/nope", routes))).toBe(false);
  });
});

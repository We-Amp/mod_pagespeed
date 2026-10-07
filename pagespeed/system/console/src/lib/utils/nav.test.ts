// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { routes, type NavGroup } from "$lib/routes";
import { isNavActive, navKeyHint } from "./nav";

const labels = (group: NavGroup, isGlobal: boolean) =>
  routes.filter((r) => r.group === group && r.hidden !== true && (r.globalOnly !== true || isGlobal)).map((r) => r.label);

describe("the sidebar", () => {
  it("lists the primary pages by group, plus Support and About", () => {
    expect(labels("overview", true)).toEqual(["Overview", "Savings", "URLs"]);
    expect(labels("module", true)).toEqual(["Statistics", "Histograms", "Caches", "Configuration"]);
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

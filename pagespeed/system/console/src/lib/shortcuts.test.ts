// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { routes } from "./routes";
import { parseHash } from "./utils/hash-route";
import { GO_KEYS, GO_TIMEOUT_MS, SHORTCUT_HELP, ShortcutReader } from "./shortcuts";

const key = (k: string, extra = {}) => ({ key: k, ...extra });

describe("ShortcutReader", () => {
  it("single keys: help, refresh, search", () => {
    const r = new ShortcutReader();
    expect(r.read(key("?"), 0)).toEqual({ kind: "help" });
    expect(r.read(key("r"), 0)).toEqual({ kind: "refresh" });
    expect(r.read(key("/"), 0)).toEqual({ kind: "search" });
  });

  it("g then a letter goes to a page", () => {
    const r = new ShortcutReader();
    expect(r.read(key("g"), 0)).toBeNull();
    expect(r.read(key("s"), 500)).toEqual({ kind: "go", path: "#/statistics" });
    expect(r.read(key("g"), 1000)).toBeNull();
    expect(r.read(key("g"), 1200)).toEqual({ kind: "go", path: "#/graphs" });
    expect(r.read(key("g"), 2000)).toBeNull();
    expect(r.read(key("l"), 2100)).toEqual({ kind: "go", path: "#/logs" });
  });

  it("the second key must come soon, and must be one of the page letters", () => {
    const r = new ShortcutReader();
    r.read(key("g"), 0);
    expect(r.read(key("s"), GO_TIMEOUT_MS + 1)).toBeNull();
    r.read(key("g"), 10_000);
    expect(r.read(key("x"), 10_100)).toBeNull();
    expect(r.read(key("s"), 10_200)).toBeNull();
  });

  it("nothing while typing in a field or with a modifier held", () => {
    const r = new ShortcutReader();
    expect(r.read(key("?", { editable: true }), 0)).toBeNull();
    expect(r.read(key("r", { ctrlKey: true }), 0)).toBeNull();
    expect(r.read(key("r", { metaKey: true }), 0)).toBeNull();
    expect(r.read(key("g", { altKey: true }), 0)).toBeNull();
    r.read(key("g"), 0);
    expect(r.read(key("s", { editable: true }), 100)).toBeNull();
    expect(r.read(key("s"), 200)).toBeNull();
  });

  it("every page letter is listed in the help", () => {
    for (const letter of Object.keys(GO_KEYS)) {
      expect(SHORTCUT_HELP.some((h) => h.keys === `g ${letter}`)).toBe(true);
    }
    expect(Object.keys(GO_KEYS).sort()).toEqual(["a", "c", "d", "g", "h", "l", "m", "o", "s", "u", "v"]);
  });

  it("the help for 'r' names the pages it does not refresh (Configuration, About, Support have no poller)", () => {
    const refresh = SHORTCUT_HELP.find((h) => h.keys === "r");
    expect(refresh?.description).toMatch(/configuration/i);
    expect(refresh?.description).toMatch(/about/i);
    expect(refresh?.description).toMatch(/support/i);
  });
});

describe("GO_KEYS", () => {
  it("every target path is a real route: a typo cannot land silently on the fallback route", () => {
    const paths = new Set(routes.map((r) => r.path));
    for (const [letter, path] of Object.entries(GO_KEYS)) {
      expect(paths.has(parseHash(path).path), `g ${letter} -> ${path}`).toBe(true);
    }
  });
});

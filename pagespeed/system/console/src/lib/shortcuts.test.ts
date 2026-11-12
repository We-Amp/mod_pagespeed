// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { routes } from "./routes";
import { parseHash } from "./utils/hash-route";
import {
  GO_KEYS,
  GO_TIMEOUT_MS,
  SHORTCUTS_STORAGE_KEY,
  ShortcutReader,
  goKeysFor,
  setShortcutsOn,
  shortcutHelp,
  shortcutsOn,
  takesTyping,
} from "./shortcuts";

const SHORTCUT_HELP = shortcutHelp(true);

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
    expect(Object.keys(GO_KEYS).sort()).toEqual(["a", "c", "d", "g", "h", "l", "m", "o", "p", "s", "u", "v"]);
  });

  it("g then p goes to Rule packs", () => {
    const r = new ShortcutReader();
    r.read(key("g"), 0);
    expect(r.read(key("p"), 100)).toEqual({ kind: "go", path: "#/rule-packs" });
  });

  it("the reader answers only the pages it is given", () => {
    const r = new ShortcutReader();
    r.goKeys = goKeysFor(false);
    r.read(key("g"), 0);
    expect(r.read(key("u"), 100)).toBeNull();
    r.read(key("g"), 200);
    expect(r.read(key("s"), 300)).toEqual({ kind: "go", path: "#/statistics" });
  });

  it("the help for 'r' names the pages with nothing to refresh, and no longer sends Configuration to a button of its own", () => {
    const refresh = SHORTCUT_HELP.find((h) => h.keys === "r");
    expect(refresh?.description).toMatch(/about/i);
    expect(refresh?.description).toMatch(/support/i);
    // Configuration has the shared Refresh button, and "r" refreshes it too.
    expect(refresh?.description).not.toMatch(/configuration/i);
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

describe("the list for each console", () => {
  const goRows = (isGlobal: boolean) => shortcutHelp(isGlobal).filter((h) => h.keys.startsWith("g "));

  it("the whole-server console lists every page shortcut", () => {
    expect(goRows(true).map((h) => h.keys)).toEqual(Object.keys(GO_KEYS).map((k) => `g ${k}`));
    expect(goRows(true).find((h) => h.keys === "g p")?.description).toBe("Go to Rule packs");
  });

  it("a per-host console leaves out the pages only the whole-server console has", () => {
    const keys = goRows(false).map((h) => h.keys);
    expect(keys).not.toContain("g u");
    expect(keys).toContain("g s");
    expect(keys).toContain("g p");
    expect(Object.values(goKeysFor(false))).not.toContain("#/urls");
  });

  it("every listed page is in this console's sidebar, or lives under a page that is", () => {
    for (const isGlobal of [true, false]) {
      const sidebar = new Set(routes.filter((r) => r.hidden !== true && (r.globalOnly !== true || isGlobal)).map((r) => r.path));
      for (const target of Object.values(goKeysFor(isGlobal))) {
        const route = routes.find((r) => r.path === parseHash(target).path)!;
        const home = route.hidden === true ? (isGlobal ? route.parent : (route.parentPerVhost ?? route.parent)) : route.path;
        expect(sidebar.has(home!), `${target} on ${isGlobal ? "whole-server" : "per-host"}`).toBe(true);
      }
    }
  });

  it("the help rows other than the page shortcuts are the same on both", () => {
    const rest = (isGlobal: boolean) => shortcutHelp(isGlobal).filter((h) => !h.keys.startsWith("g "));
    expect(rest(false)).toEqual(rest(true));
  });
});

describe("takesTyping", () => {
  it("text fields, text areas, selects and editable content take the keys", () => {
    for (const type of ["text", "search", "url", "email", "number", "password", "tel", "date", undefined]) {
      expect(takesTyping({ tagName: "INPUT", type }), `input ${type}`).toBe(true);
    }
    expect(takesTyping({ tagName: "TEXTAREA" })).toBe(true);
    expect(takesTyping({ tagName: "SELECT" })).toBe(true);
    expect(takesTyping({ tagName: "DIV", isContentEditable: true })).toBe(true);
  });

  it("checkboxes, radio buttons, buttons and links leave the keys to the shortcuts", () => {
    for (const type of ["checkbox", "radio", "button", "submit", "reset", "range", "color", "file", "CHECKBOX"]) {
      expect(takesTyping({ tagName: "INPUT", type }), `input ${type}`).toBe(false);
    }
    expect(takesTyping({ tagName: "BUTTON" })).toBe(false);
    expect(takesTyping({ tagName: "A" })).toBe(false);
    expect(takesTyping({ tagName: "DIV", isContentEditable: false })).toBe(false);
  });
});

describe("the on/off switch", () => {
  function memoryStorage(): Storage {
    const m = new Map<string, string>();
    return {
      get length() {
        return m.size;
      },
      clear: () => m.clear(),
      getItem: (k) => m.get(k) ?? null,
      key: (i) => [...m.keys()][i] ?? null,
      removeItem: (k) => void m.delete(k),
      setItem: (k, v) => void m.set(k, String(v)),
    };
  }

  const throwing = {
    getItem() {
      throw new Error("denied");
    },
    setItem() {
      throw new Error("denied");
    },
    removeItem() {
      throw new Error("denied");
    },
  } as unknown as Storage;

  it("is on in a fresh browser", () => {
    expect(shortcutsOn(memoryStorage())).toBe(true);
    expect(shortcutsOn(null)).toBe(true);
  });

  it("remembers off, and on again", () => {
    const store = memoryStorage();
    setShortcutsOn(false, store);
    expect(store.getItem(SHORTCUTS_STORAGE_KEY)).toBe("off");
    expect(shortcutsOn(store)).toBe(false);
    setShortcutsOn(true, store);
    expect(store.getItem(SHORTCUTS_STORAGE_KEY)).toBeNull();
    expect(shortcutsOn(store)).toBe(true);
  });

  it("storage that throws means on, and a choice that is not remembered", () => {
    expect(shortcutsOn(throwing)).toBe(true);
    expect(() => setShortcutsOn(false, throwing)).not.toThrow();
    expect(() => setShortcutsOn(true, throwing)).not.toThrow();
  });
});

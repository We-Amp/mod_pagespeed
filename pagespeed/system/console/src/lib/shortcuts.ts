// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The console's keyboard shortcuts. Single keys without modifiers, never
 * while typing in a field, so browser and assistive-technology shortcuts are
 * untouched. "g" starts a two-key "go to page" sequence. The viewer can turn
 * them all off (speech input dictates letters); the choice is remembered in
 * this browser.
 */

import { routes } from "./routes";
import { parseHash } from "./utils/hash-route";

export type ShortcutAction =
  | { kind: "help" }
  | { kind: "refresh" }
  | { kind: "search" }
  | { kind: "go"; path: string };

export interface KeyInput {
  key: string;
  ctrlKey?: boolean;
  metaKey?: boolean;
  altKey?: boolean;
  /** The key went to a text field, select or editable element. */
  editable?: boolean;
}

export const GO_KEYS: Readonly<Record<string, string>> = {
  o: "#/overview",
  v: "#/savings",
  u: "#/urls",
  l: "#/logs",
  m: "#/logs?source=module",
  s: "#/statistics",
  g: "#/graphs",
  h: "#/histograms",
  a: "#/caches",
  c: "#/configuration",
  p: "#/rule-packs",
  d: "#/optimizer",
};

const GO_NAMES: Readonly<Record<string, string>> = {
  o: "Overview",
  v: "Savings",
  u: "URLs",
  l: "Logs",
  m: "Module messages (Logs)",
  s: "Statistics",
  g: "Graphs (Statistics)",
  h: "Histograms",
  a: "Caches",
  c: "Configuration",
  p: "Rule packs",
  d: "Optimizer status",
};

/** How long "g" waits for its second key. */
export const GO_TIMEOUT_MS = 1500;

/**
 * The "g x" pages this console has: a page the sidebar lists only on the
 * whole-server console (URLs) has no shortcut on a per-host one, where it
 * only explains that it is not available.
 */
export function goKeysFor(isGlobal: boolean): Readonly<Record<string, string>> {
  const keys: Record<string, string> = {};
  for (const [letter, target] of Object.entries(GO_KEYS)) {
    const route = routes.find((r) => r.path === parseHash(target).path);
    if (route !== undefined && (route.globalOnly !== true || isGlobal)) keys[letter] = target;
  }
  return keys;
}

/** The list the shortcuts dialog shows, for the console that is open. */
export function shortcutHelp(isGlobal: boolean): ReadonlyArray<{ keys: string; description: string }> {
  return [
    { keys: "?", description: "Show this list" },
    { keys: "r", description: "Refresh the page's data now, as the Refresh button does (About and Support have no data to refresh)" },
    { keys: "/", description: "Jump to the page's search box" },
    ...Object.keys(goKeysFor(isGlobal)).map((k) => ({ keys: `g ${k}`, description: `Go to ${GO_NAMES[k]}` })),
    { keys: "Esc", description: "Close this list" },
  ];
}

/**
 * Whether a key pressed on `el` goes into the element rather than to the
 * page: text fields, text areas, selects (typing picks an option) and
 * editable content. A checkbox, radio button, button, range or colour input
 * takes no letters, so shortcuts still work from it.
 */
export function takesTyping(el: { tagName: string; type?: string; isContentEditable?: boolean }): boolean {
  if (el.isContentEditable === true) return true;
  switch (el.tagName) {
    case "TEXTAREA":
    case "SELECT":
      return true;
    case "INPUT":
      return !NON_TEXT_INPUTS.has((el.type ?? "text").toLowerCase());
    default:
      return false;
  }
}

const NON_TEXT_INPUTS: ReadonlySet<string> = new Set([
  "checkbox",
  "radio",
  "button",
  "submit",
  "reset",
  "image",
  "range",
  "color",
  "file",
  "hidden",
]);

export const SHORTCUTS_STORAGE_KEY = "pagespeed.console.shortcuts";

function storage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** True unless the viewer turned the shortcuts off in this browser. */
export function shortcutsOn(store: Storage | null = storage()): boolean {
  try {
    return store?.getItem(SHORTCUTS_STORAGE_KEY) !== "off";
  } catch {
    return true;
  }
}

/** Remembers the choice; storage that is missing or throws only means it is not remembered. */
export function setShortcutsOn(on: boolean, store: Storage | null = storage()): void {
  try {
    if (on) store?.removeItem(SHORTCUTS_STORAGE_KEY);
    else store?.setItem(SHORTCUTS_STORAGE_KEY, "off");
  } catch {
    // Not remembered; this page view still follows the choice.
  }
}

export class ShortcutReader {
  #goSince: number | null = null;

  /** The "g x" pages to answer: by default every one; the shell passes this console's. */
  goKeys: Readonly<Record<string, string>> = GO_KEYS;

  read(k: KeyInput, now: number): ShortcutAction | null {
    if (k.ctrlKey || k.metaKey || k.altKey || k.editable) {
      this.#goSince = null;
      return null;
    }
    if (this.#goSince !== null) {
      const waiting = now - this.#goSince <= GO_TIMEOUT_MS;
      this.#goSince = null;
      if (waiting) {
        const path = Object.prototype.hasOwnProperty.call(this.goKeys, k.key) ? this.goKeys[k.key] : undefined;
        return path === undefined ? null : { kind: "go", path };
      }
    }
    switch (k.key) {
      case "?":
        return { kind: "help" };
      case "r":
        return { kind: "refresh" };
      case "/":
        return { kind: "search" };
      case "g":
        this.#goSince = now;
        return null;
      default:
        return null;
    }
  }
}

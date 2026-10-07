// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The console's keyboard shortcuts. Single keys without modifiers, never
 * while typing in a field, so browser and assistive-technology shortcuts are
 * untouched. "g" starts a two-key "go to page" sequence.
 */

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
  s: "#/statistics",
  g: "#/graphs",
  h: "#/histograms",
  a: "#/caches",
  c: "#/configuration",
  m: "#/logs?source=module",
  d: "#/optimizer",
  l: "#/logs",
};

const GO_NAMES: Readonly<Record<string, string>> = {
  o: "Overview",
  v: "Savings",
  u: "URLs",
  s: "Statistics",
  g: "Graphs (Statistics)",
  h: "Histograms",
  a: "Caches",
  c: "Configuration",
  m: "Module messages (Logs)",
  d: "Optimizer status",
  l: "Logs",
};

/** How long "g" waits for its second key. */
export const GO_TIMEOUT_MS = 1500;

export const SHORTCUT_HELP: ReadonlyArray<{ keys: string; description: string }> = [
  { keys: "?", description: "Show this list" },
  { keys: "r", description: "Refresh the page's data now (Configuration, About and Support have none to refresh)" },
  { keys: "/", description: "Jump to the page's search box" },
  ...Object.keys(GO_KEYS).map((k) => ({ keys: `g ${k}`, description: `Go to ${GO_NAMES[k]}` })),
  { keys: "Esc", description: "Close this list" },
];

export class ShortcutReader {
  #goSince: number | null = null;

  read(k: KeyInput, now: number): ShortcutAction | null {
    if (k.ctrlKey || k.metaKey || k.altKey || k.editable) {
      this.#goSince = null;
      return null;
    }
    if (this.#goSince !== null) {
      const waiting = now - this.#goSince <= GO_TIMEOUT_MS;
      this.#goSince = null;
      if (waiting) {
        const path = Object.prototype.hasOwnProperty.call(GO_KEYS, k.key) ? GO_KEYS[k.key] : undefined;
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

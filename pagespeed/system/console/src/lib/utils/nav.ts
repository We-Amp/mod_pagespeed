// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** Sidebar helpers: the shortcut hint beside an entry, and which entry is current. */

import type { Route } from "$lib/routes";
import { GO_KEYS } from "$lib/shortcuts";

/** The "g x" shortcut that goes exactly to `path`, or null when it has none. */
export function navKeyHint(path: string): string | null {
  for (const [letter, target] of Object.entries(GO_KEYS)) {
    if (target === path) return `g ${letter}`;
  }
  return null;
}

/**
 * Whether a sidebar entry is the current page: its own route, or a page
 * without an entry of its own that lives under it (Graphs under Statistics,
 * a URL's detail under URLs).
 */
export function isNavActive(entry: Route, currentPath: string, all: readonly Route[]): boolean {
  if (currentPath === entry.path) return true;
  const current = all.find((r) => r.path === currentPath);
  return current !== undefined && current.parent === entry.path;
}

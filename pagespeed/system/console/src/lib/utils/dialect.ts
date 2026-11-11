// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Which web server's configuration syntax a snippet is shown in. The admin
 * endpoints do not say which server they run in, so the console offers one
 * tab per syntax and preselects: the tab chosen in this page view, else the
 * server named by the `Server` response header of the console's own
 * requests, else the tab remembered from an earlier visit. The header is a
 * hint (a proxy in front may have written it), so the choice stays with the
 * operator. Storage that refuses (private mode, blocked site data) costs
 * only the memory across visits.
 */

export type Dialect = "apache" | "nginx" | "iis" | "envoy";

/** Tab order and labels. */
export const DIALECTS: readonly { id: Dialect; label: string }[] = [
  { id: "apache", label: "Apache" },
  { id: "nginx", label: "nginx" },
  { id: "iis", label: "IIS" },
  { id: "envoy", label: "Envoy" },
];

export const DIALECT_STORAGE_KEY = "pagespeed.snippet.dialect";

const isDialect = (v: unknown): v is Dialect => DIALECTS.some((d) => d.id === v);

/** The server a `Server` header names, or null when it is absent or names none of them. */
export function dialectOfServer(header: string | null | undefined): Dialect | null {
  const m = /apache|nginx|microsoft-iis|envoy/i.exec(header ?? "");
  if (m === null) return null;
  const name = m[0].toLowerCase();
  return name === "microsoft-iis" ? "iis" : (name as Dialect);
}

let detected: Dialect | null = null;
let chosen: Dialect | null = null;
const listeners = new Set<() => void>();

function defaultStorage(): Storage | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

/** Called for every answered request with its `Server` header. */
export function noteServer(header: string | null | undefined): void {
  const d = dialectOfServer(header);
  if (d !== null) detected = d;
}

/** The server the console's own requests named, if any. */
export function detectedDialect(): Dialect | null {
  return detected;
}

/** The tab to preselect; null when nothing is chosen, detected or remembered. */
export function preferredDialect(storage: Storage | null = defaultStorage()): Dialect | null {
  if (chosen !== null) return chosen;
  if (detected !== null) return detected;
  try {
    const stored = storage?.getItem(DIALECT_STORAGE_KEY);
    return isDialect(stored) ? stored : null;
  } catch {
    return null;
  }
}

/** The operator picked a tab: it holds for this page view and is remembered. */
export function chooseDialect(dialect: Dialect, storage: Storage | null = defaultStorage()): void {
  chosen = dialect;
  try {
    storage?.setItem(DIALECT_STORAGE_KEY, dialect);
  } catch {
    // Ignore: the choice holds for this page view.
  }
  for (const fn of listeners) fn();
}

/** Calls `fn` on every choice, so every snippet on the page follows it; returns the unsubscribe. */
export function onDialectChoice(fn: () => void): () => void {
  listeners.add(fn);
  return () => {
    listeners.delete(fn);
  };
}

/** Forget the page view's state (tests). */
export function resetDialectForTesting(): void {
  detected = null;
  chosen = null;
}

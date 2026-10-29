// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Leaves the server has turned off. A server administrator (or a proxy in
 * front of the console) may refuse a read-only leaf with 401/403/410 --
 * the view is switched off on purpose, not broken. The client records such a
 * leaf here the first time it is refused: later reads of it fail at once,
 * without another request, so nothing keeps asking, and the sidebar can
 * mark the page that reads it. A reload asks again.
 */

/** Statuses that mean "this leaf is turned off" (404 only on module leaves; see AdminApiClient). */
export const UNAVAILABLE_STATUSES: ReadonlySet<number> = new Set([401, 403, 404, 410]);

/**
 * The identity of a leaf: its path without the query. The cache leaf keeps
 * its parameter names, because `/cache` (the structure), `/cache?url=` (one
 * entry) and `/cache?physical_caches` are different views of one path.
 */
export function leafKey(path: string): string {
  const q = path.indexOf("?");
  if (q === -1) return path;
  const base = path.slice(0, q);
  if (base !== "/cache") return base;
  const names = [...new URLSearchParams(path.slice(q + 1)).keys()].sort();
  return `${base}?${names.join("&")}`;
}

export class UnavailableLeaves {
  private readonly leaves = new Map<string, number>();
  private readonly listeners = new Set<() => void>();

  mark(key: string, status: number): void {
    if (this.leaves.has(key)) return;
    this.leaves.set(key, status);
    for (const fn of this.listeners) fn();
  }

  statusOf(key: string): number | undefined {
    return this.leaves.get(key);
  }

  has(key: string): boolean {
    return this.leaves.has(key);
  }

  /** Whether every one of `keys` is off (false for an empty list). */
  allOff(keys: readonly string[] | undefined): boolean {
    return keys !== undefined && keys.length > 0 && keys.every((k) => this.leaves.has(k));
  }

  /** Calls `fn` whenever a leaf is newly marked; returns an unsubscribe. */
  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => {
      this.listeners.delete(fn);
    };
  }

  reset(): void {
    this.leaves.clear();
    for (const fn of this.listeners) fn();
  }
}

export const unavailable = new UnavailableLeaves();

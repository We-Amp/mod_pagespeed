// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { HostSet, LENS_STORAGE_KEY, normalizeLensHost, saveLens, type HostRank } from "$lib/utils/host-lens";

/**
 * The console's host lens: the host the whole-server console narrows its
 * per-host views to (null: all hosts) and the hosts it has seen. Pages
 * report hosts from inside their fetchers (observe) and hear about a new
 * selection through onChange -- a plain callback, never an $effect.
 */
export class HostLens {
  host = $state<string | null>(null);
  known = $state.raw<string[]>([]);
  readonly #hosts = new HostSet();
  readonly #listeners = new Set<(host: string | null) => void>();
  readonly #storageKey: string;

  /** `storageKey`: this console's own key (lensStorageKey). */
  constructor(initial: string | null, storageKey: string = LENS_STORAGE_KEY) {
    this.host = normalizeLensHost(initial);
    this.#storageKey = storageKey;
  }

  /** Selects one host, or all hosts with null, and remembers it. A value outside the grammar is ignored. */
  select(value: string | null): void {
    const host = value === null ? null : normalizeLensHost(value);
    if (value !== null && host === null) return;
    if (host === this.host) return;
    this.host = host;
    saveLens(host, undefined, this.#storageKey);
    for (const fn of this.#listeners) fn(host);
  }

  /** Hosts a page has read (see HostRank). */
  observe(hosts: Iterable<unknown>, rank: HostRank): void {
    if (this.#hosts.add(hosts, rank)) this.known = this.#hosts.list();
  }

  /** What the control offers: the known hosts, plus the selected one. */
  get options(): string[] {
    const host = this.host;
    return host === null || this.known.includes(host) ? this.known : [...this.known, host].sort();
  }

  /** Calls `fn` on every new selection; returns the unsubscribe (hand it to onDestroy). */
  onChange(fn: (host: string | null) => void): () => void {
    this.#listeners.add(fn);
    return () => {
      this.#listeners.delete(fn);
    };
  }
}

/** The host a page applies: the selection on the whole-server console; never on a per-vhost one. */
export function activeLensHost(scope: { isGlobal: boolean }, lens: HostLens): string | null {
  return scope.isGlobal ? lens.host : null;
}

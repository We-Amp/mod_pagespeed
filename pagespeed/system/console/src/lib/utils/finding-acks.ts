// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Which overview findings the operator acknowledged, remembered across page
 * loads in localStorage. Same storage-failure tolerance as support-panel.ts:
 * a browser that refuses storage (private mode, blocked site data) still
 * gets acknowledgements for the session. What comes back from storage is
 * checked: only finding ids (a fixed vocabulary of rule ids), at most
 * MAX_ACKS of them.
 */

export const FINDING_ACKS_STORAGE_KEY = "pagespeed.findings.acknowledged";
export const MAX_ACKS = 64;

const FINDING_ID = /^[a-z][a-z0-9-]{0,63}$/;

export interface AckStore {
  has(id: string): boolean;
  add(id: string): void;
  delete(id: string): void;
  ids(): string[];
}

/** Acknowledgements in memory only. */
export function memoryAckStore(initial: Iterable<string> = []): AckStore {
  const set = new Set<string>();
  const add = (id: string) => {
    if (FINDING_ID.test(id) && (set.has(id) || set.size < MAX_ACKS)) set.add(id);
  };
  for (const id of initial) add(id);
  return {
    has: (id) => set.has(id),
    add,
    delete: (id) => {
      set.delete(id);
    },
    ids: () => [...set],
  };
}

/**
 * Acknowledgements remembered in `storage` (localStorage by default). Each
 * write re-reads the stored list and applies only its own change, so a
 * second open tab does not overwrite this one's acknowledgements (and the
 * reverse).
 */
export function createAckStore(storage: Storage | null = defaultStorage()): AckStore {
  const memory = memoryAckStore(load(storage));
  const write = (change: (stored: AckStore) => void) => {
    if (storage === null) return;
    try {
      const stored = memoryAckStore(load(storage));
      change(stored);
      storage.setItem(FINDING_ACKS_STORAGE_KEY, JSON.stringify(stored.ids()));
    } catch {
      // Ignore: the acknowledgement holds for this page view, it just won't persist.
    }
  };
  return {
    has: memory.has,
    add: (id) => {
      if (memory.has(id)) return;
      memory.add(id);
      if (memory.has(id)) write((stored) => stored.add(id));
    },
    delete: (id) => {
      if (!memory.has(id)) return;
      memory.delete(id);
      write((stored) => stored.delete(id));
    },
    ids: memory.ids,
  };
}

function load(storage: Storage | null): string[] {
  try {
    const raw = storage?.getItem(FINDING_ACKS_STORAGE_KEY);
    if (typeof raw !== "string") return [];
    const parsed: unknown = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.filter((v): v is string => typeof v === "string") : [];
  } catch {
    return [];
  }
}

/** `localStorage` when the environment has one; merely touching it can throw. */
function defaultStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

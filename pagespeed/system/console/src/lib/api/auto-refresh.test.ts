// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { AUTO_REFRESH_STORAGE_KEY, autoRefreshOn, onAutoRefreshChange, setAutoRefresh } from "./auto-refresh";

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
    setItem: (k, v) => void m.set(k, v),
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

describe("auto-refresh choice", () => {
  it("is on in a fresh browser", () => {
    expect(autoRefreshOn(memoryStorage())).toBe(true);
    expect(autoRefreshOn(null)).toBe(true);
  });

  it("remembers a pause until it is turned back on", () => {
    const store = memoryStorage();
    setAutoRefresh(false, store);
    expect(store.getItem(AUTO_REFRESH_STORAGE_KEY)).toBe("off");
    expect(autoRefreshOn(store)).toBe(false);
    setAutoRefresh(true, store);
    expect(store.length).toBe(0);
    expect(autoRefreshOn(store)).toBe(true);
  });

  it("tells every listener, also when storage refuses", () => {
    const seen: boolean[] = [];
    const off = onAutoRefreshChange((on) => seen.push(on));
    setAutoRefresh(false, throwing);
    setAutoRefresh(true, throwing);
    off();
    setAutoRefresh(false, throwing);
    expect(seen).toEqual([false, true]);
    expect(autoRefreshOn(throwing)).toBe(true);
  });
});

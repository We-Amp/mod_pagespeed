// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { FINDING_ACKS_STORAGE_KEY, MAX_ACKS, createAckStore, memoryAckStore } from "./finding-acks";

function fakeStorage(): Storage {
  const m = new Map<string, string>();
  return {
    getItem: (k: string) => m.get(k) ?? null,
    setItem: (k: string, v: string) => void m.set(k, v),
    removeItem: (k: string) => void m.delete(k),
    clear: () => m.clear(),
    key: () => null,
    get length() {
      return m.size;
    },
  } as Storage;
}

describe("memoryAckStore", () => {
  it("adds, answers and deletes finding ids", () => {
    const s = memoryAckStore(["admin-exposed"]);
    expect(s.has("admin-exposed")).toBe(true);
    s.add("dirty-build");
    expect(s.ids()).toEqual(["admin-exposed", "dirty-build"]);
    s.delete("admin-exposed");
    expect(s.has("admin-exposed")).toBe(false);
  });
});

describe("createAckStore", () => {
  it("remembers acknowledgements across page loads", () => {
    const storage = fakeStorage();
    createAckStore(storage).add("admin-exposed");
    const reloaded = createAckStore(storage);
    expect(reloaded.has("admin-exposed")).toBe(true);
    reloaded.delete("admin-exposed");
    expect(createAckStore(storage).has("admin-exposed")).toBe(false);
  });

  it("two tabs keep each other's acknowledgements: a write re-reads the stored list", () => {
    const storage = fakeStorage();
    const tabA = createAckStore(storage);
    const tabB = createAckStore(storage);
    tabA.add("admin-exposed");
    tabB.add("dirty-build");
    expect(createAckStore(storage).ids().sort()).toEqual(["admin-exposed", "dirty-build"]);
    tabA.delete("admin-exposed");
    expect(createAckStore(storage).ids()).toEqual(["dirty-build"]);
  });

  it("works for the session when storage throws", () => {
    const throwing = {
      getItem: () => {
        throw new Error("denied");
      },
      setItem: () => {
        throw new Error("denied");
      },
    } as unknown as Storage;
    const s = createAckStore(throwing);
    expect(() => s.add("admin-exposed")).not.toThrow();
    expect(s.has("admin-exposed")).toBe(true);
    s.delete("admin-exposed");
    expect(s.has("admin-exposed")).toBe(false);
  });

  it("works in memory without any storage", () => {
    const s = createAckStore(null);
    s.add("admin-exposed");
    expect(s.has("admin-exposed")).toBe(true);
  });

  it("drops stored junk: non-JSON, non-arrays, non-strings and malformed ids", () => {
    for (const raw of ["not json", '{"a":1}', "42", "null"]) {
      const storage = fakeStorage();
      storage.setItem(FINDING_ACKS_STORAGE_KEY, raw);
      expect(createAckStore(storage).ids(), raw).toEqual([]);
    }
    const storage = fakeStorage();
    storage.setItem(
      FINDING_ACKS_STORAGE_KEY,
      JSON.stringify(["ok-id", 5, "Bad Id", "<script>", "x".repeat(70), "admin-exposed"]),
    );
    expect(createAckStore(storage).ids()).toEqual(["ok-id", "admin-exposed"]);
  });

  it("keeps at most MAX_ACKS ids and refuses a malformed one", () => {
    const storage = fakeStorage();
    storage.setItem(FINDING_ACKS_STORAGE_KEY, JSON.stringify(Array.from({ length: 100 }, (_, i) => `id-${i}`)));
    const s = createAckStore(storage);
    expect(s.ids()).toHaveLength(MAX_ACKS);
    s.add("one-more");
    expect(s.has("one-more")).toBe(false);
    const fresh = createAckStore(fakeStorage());
    fresh.add("Bad Id");
    expect(fresh.ids()).toEqual([]);
  });
});

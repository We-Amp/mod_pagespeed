// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import {
  SUPPORT_DISMISSED_STORAGE_KEY,
  loadSupportDismissed,
  saveSupportDismissed,
  supportSentence,
} from "./support-panel";

/** Minimal in-memory Storage stand-in. */
function fakeStorage(initial: Record<string, string> = {}): Storage {
  const map = new Map(Object.entries(initial));
  return {
    get length() {
      return map.size;
    },
    clear: () => map.clear(),
    getItem: (k: string) => (map.has(k) ? map.get(k)! : null),
    key: (i: number) => [...map.keys()][i] ?? null,
    removeItem: (k: string) => void map.delete(k),
    setItem: (k: string, v: string) => void map.set(k, String(v)),
  } as Storage;
}

/** Storage that throws on every access, like Safari with cookies blocked. */
function throwingStorage(): Storage {
  const boom = () => {
    throw new Error("SecurityError: storage is not available");
  };
  return {
    get length(): number {
      return boom();
    },
    clear: boom,
    getItem: boom,
    key: boom,
    removeItem: boom,
    setItem: boom,
  } as unknown as Storage;
}

describe("support-panel dismissal", () => {
  it("defaults to not dismissed", () => {
    expect(loadSupportDismissed(fakeStorage())).toBe(false);
  });

  it("round-trips the dismissed state", () => {
    const storage = fakeStorage();
    saveSupportDismissed(true, storage);
    expect(storage.getItem(SUPPORT_DISMISSED_STORAGE_KEY)).toBe("1");
    expect(loadSupportDismissed(storage)).toBe(true);
    saveSupportDismissed(false, storage);
    expect(loadSupportDismissed(storage)).toBe(false);
  });

  it("tolerates storage that throws, defaulting to not dismissed", () => {
    expect(loadSupportDismissed(throwingStorage())).toBe(false);
    expect(() => saveSupportDismissed(true, throwingStorage())).not.toThrow();
  });

  it("tolerates absent storage", () => {
    expect(loadSupportDismissed(null)).toBe(false);
    expect(() => saveSupportDismissed(true, null)).not.toThrow();
  });
});

describe("supportSentence", () => {
  it("does not double the full stop after a vendor name that ends in one", () => {
    const s = supportSentence("mod_pagespeed", "We-Amp B.V.");
    expect(s).toBe(
      "mod_pagespeed is developed and maintained by We-Amp B.V. A support subscription funds that work; " +
        "the software is fully functional without one.",
    );
    expect(s).not.toContain("..");
  });
  it("ends the first sentence when the vendor name does not", () => {
    expect(supportSentence("x", "Acme")).toMatch(/^x is developed and maintained by Acme\. A support/);
  });
});

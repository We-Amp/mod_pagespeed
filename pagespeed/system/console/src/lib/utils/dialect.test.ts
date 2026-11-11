// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { beforeEach, describe, expect, it } from "vitest";
import {
  DIALECT_STORAGE_KEY,
  chooseDialect,
  detectedDialect,
  dialectOfServer,
  noteServer,
  onDialectChoice,
  preferredDialect,
  resetDialectForTesting,
} from "./dialect";

function memoryStorage(initial: Record<string, string> = {}): Storage {
  const map = new Map(Object.entries(initial));
  return {
    getItem: (k: string) => map.get(k) ?? null,
    setItem: (k: string, v: string) => void map.set(k, v),
    removeItem: (k: string) => void map.delete(k),
    clear: () => map.clear(),
    key: () => null,
    get length() {
      return map.size;
    },
  };
}

const refusing = {
  getItem: () => {
    throw new Error("blocked");
  },
  setItem: () => {
    throw new Error("blocked");
  },
} as unknown as Storage;

beforeEach(() => resetDialectForTesting());

describe("the web server a Server header names", () => {
  it("reads the four servers, whatever the version or case", () => {
    expect(dialectOfServer("Apache/2.4.62 (Debian)")).toBe("apache");
    expect(dialectOfServer("nginx/1.26.2")).toBe("nginx");
    expect(dialectOfServer("Microsoft-IIS/10.0")).toBe("iis");
    expect(dialectOfServer("envoy")).toBe("envoy");
    expect(dialectOfServer("NGINX")).toBe("nginx");
  });

  it("an absent header or another server names none", () => {
    expect(dialectOfServer(null)).toBeNull();
    expect(dialectOfServer(undefined)).toBeNull();
    expect(dialectOfServer("")).toBeNull();
    expect(dialectOfServer("cloudflare")).toBeNull();
  });
});

describe("the preselected snippet tab", () => {
  it("nothing chosen, detected or remembered: no preference", () => {
    expect(preferredDialect(memoryStorage())).toBeNull();
  });

  it("the remembered tab, when the header names no server", () => {
    noteServer(null);
    expect(detectedDialect()).toBeNull();
    expect(preferredDialect(memoryStorage({ [DIALECT_STORAGE_KEY]: "iis" }))).toBe("iis");
  });

  it("a stored value outside the vocabulary is ignored", () => {
    expect(preferredDialect(memoryStorage({ [DIALECT_STORAGE_KEY]: "<script>" }))).toBeNull();
  });

  it("the detected server comes before the remembered tab, and a later header without a name keeps it", () => {
    noteServer("nginx/1.26.2");
    noteServer(null);
    expect(detectedDialect()).toBe("nginx");
    expect(preferredDialect(memoryStorage({ [DIALECT_STORAGE_KEY]: "iis" }))).toBe("nginx");
  });

  it("a tab chosen in this page view comes first, is remembered, and is told to every listener", () => {
    const storage = memoryStorage();
    noteServer("nginx");
    let told = 0;
    const off = onDialectChoice(() => told++);
    chooseDialect("apache", storage);
    expect(preferredDialect(storage)).toBe("apache");
    expect(storage.getItem(DIALECT_STORAGE_KEY)).toBe("apache");
    expect(told).toBe(1);
    off();
    chooseDialect("iis", storage);
    expect(told).toBe(1);
  });

  it("storage that refuses costs only the memory across visits", () => {
    expect(preferredDialect(refusing)).toBeNull();
    chooseDialect("envoy", refusing);
    expect(preferredDialect(refusing)).toBe("envoy");
  });
});

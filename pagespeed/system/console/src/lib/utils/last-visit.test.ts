// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  configDigest,
  lastVisitKey,
  readVisit,
  sinceLastVisit,
  START_TOLERANCE_MS,
  VisitOnce,
  visitChanges,
  writeVisit,
  type VisitRecord,
} from "./last-visit";

const DAY = 86_400_000;
const T0 = 1_790_000_000_000;

function record(over: Partial<VisitRecord> = {}): VisitRecord {
  return {
    at: T0,
    module: "v2.8.1-6-gb33587792",
    moduleStart: T0 - 3 * DAY,
    optimizer: "2.8.1",
    optimizerStart: T0 - 3 * DAY,
    config: "aaaa",
    ...over,
  };
}

class MemoryStorage implements Storage {
  map = new Map<string, string>();
  get length() {
    return this.map.size;
  }
  clear() {
    this.map.clear();
  }
  getItem(k: string) {
    return this.map.get(k) ?? null;
  }
  key(i: number) {
    return [...this.map.keys()][i] ?? null;
  }
  removeItem(k: string) {
    this.map.delete(k);
  }
  setItem(k: string, v: string) {
    this.map.set(k, v);
  }
}

class ThrowingStorage extends MemoryStorage {
  override getItem(): string | null {
    throw new Error("SecurityError");
  }
  override setItem(): void {
    throw new Error("QuotaExceededError");
  }
}

describe("lastVisitKey", () => {
  it("keeps the whole-server console, each host, each admin path and each origin apart", () => {
    const keys = new Set([
      lastVisitKey("http://a.test", "/pagespeed_global_admin", true, ""),
      lastVisitKey("http://a.test", "/pagespeed_admin", false, "a.test:80"),
      lastVisitKey("http://a.test", "/pagespeed_admin", false, "b.test:80"),
      lastVisitKey("http://b.test", "/pagespeed_admin", false, "a.test:80"),
      lastVisitKey("http://a.test", "/other_admin", true, ""),
    ]);
    expect(keys.size).toBe(5);
  });
});

describe("configDigest", () => {
  const text = "Version: 15: on\n\nFilters\nah\tAdd Head\n\nDomain Lawyer\n";
  it("is stable and changes with the configuration", () => {
    expect(configDigest(text)).toBe(configDigest(text));
    expect(configDigest(text)).not.toBe(configDigest(text.replace("ah\tAdd Head", "cc\tCombine Css")));
  });
  it("ignores the cache invalidation time, which a cache flush moves, in each form the server prints", () => {
    // Exactly as the server ends the text: "(none)" without a line break,
    // a flush time with one, and no line at all for an unset time.
    const none = `${text}\nInvalidation Timestamp: (none)`;
    const flushed = `${text}\nInvalidation Timestamp: Mon, 05 Apr 2026 18:51:26 GMT (1775414000000)\n`;
    const flushedAgain = `${text}\nInvalidation Timestamp: Tue, 06 Apr 2026 09:00:00 GMT (1775466000000)\n`;
    const unset = text;
    expect(configDigest(flushed)).toBe(configDigest(none));
    expect(configDigest(flushedAgain)).toBe(configDigest(flushed));
    expect(configDigest(unset)).toBe(configDigest(none));
  });
});

describe("readVisit / writeVisit", () => {
  it("round-trips a record", () => {
    const s = new MemoryStorage();
    writeVisit(s, "k", record());
    expect(readVisit(s, "k")).toEqual(record());
  });
  it("no storage, nothing stored, or junk: no record", () => {
    expect(readVisit(null, "k")).toBeNull();
    const s = new MemoryStorage();
    expect(readVisit(s, "k")).toBeNull();
    s.setItem("k", "{not json");
    expect(readVisit(s, "k")).toBeNull();
    s.setItem("k", JSON.stringify({ module: "v1" }));
    expect(readVisit(s, "k")).toBeNull();
    s.setItem("k", "42");
    expect(readVisit(s, "k")).toBeNull();
  });
  it("checks each field: a wrong type reads as unknown", () => {
    const s = new MemoryStorage();
    s.setItem("k", JSON.stringify({ at: T0, module: 7, moduleStart: "x", optimizer: "x".repeat(500), optimizerStart: -1, config: {} }));
    expect(readVisit(s, "k")).toEqual({ at: T0, module: null, moduleStart: null, optimizer: null, optimizerStart: null, config: null });
  });
  it("a browser that refuses storage throws nowhere", () => {
    const s = new ThrowingStorage();
    expect(() => writeVisit(s, "k", record())).not.toThrow();
    expect(readVisit(s, "k")).toBeNull();
  });
});

describe("visitChanges", () => {
  it("nothing changed: nothing to say", () => {
    expect(visitChanges(record(), record({ at: T0 + DAY }))).toEqual([]);
  });
  it("a web server restart moves the module's start later", () => {
    expect(visitChanges(record(), record({ moduleStart: T0 + 1000 }))).toEqual(["the web server restarted or reloaded"]);
  });
  it("a start that moved within the tolerance, or earlier, is no restart", () => {
    const b = record();
    expect(visitChanges(b, record({ moduleStart: b.moduleStart! + START_TOLERANCE_MS }))).toEqual([]);
    expect(visitChanges(b, record({ optimizerStart: b.optimizerStart! - DAY }))).toEqual([]);
  });
  it("names the configuration, the module, the optimizer's restart and version, in that order", () => {
    const now = record({ moduleStart: T0 + 1, config: "bbbb", optimizerStart: T0 + 1, optimizer: "2.8.2" });
    expect(visitChanges(record(), now)).toEqual([
      "the web server restarted or reloaded",
      "the configuration changed",
      "the optimizer restarted",
      "the optimizer changed from 2.8.1 to 2.8.2",
    ]);
  });
  it("a new module release is named by its tags, a new build of the same release by its stamps", () => {
    expect(visitChanges(record({ module: "v2.8.0" }), record({ module: "v2.8.1-3-gabc" }))).toEqual([
      "the module changed from v2.8.0 to v2.8.1",
    ]);
    expect(visitChanges(record({ module: "v2.8.1-3-gabc" }), record({ module: "v2.8.1-4-gdef" }))).toEqual([
      "the module build changed from v2.8.1-3-gabc to v2.8.1-4-gdef",
    ]);
  });
  it("a new module build: the configuration text is not compared (the listing itself may differ)", () => {
    expect(visitChanges(record({ module: "v2.8.0" }), record({ module: "v2.8.1", config: "bbbb" }))).toEqual([
      "the module changed from v2.8.0 to v2.8.1",
    ]);
  });
  it("an unknown on either side claims nothing", () => {
    const unknown = record({ module: null, moduleStart: null, optimizer: null, optimizerStart: null, config: null });
    const changed = record({ module: "v9.9.9", moduleStart: T0 + DAY, optimizer: "9", optimizerStart: T0 + DAY, config: "zzzz" });
    expect(visitChanges(unknown, changed)).toEqual([]);
    expect(visitChanges(changed, unknown)).toEqual([]);
  });
});

describe("sinceLastVisit", () => {
  it("a first visit shows nothing", () => {
    expect(sinceLastVisit(null, record())).toBeNull();
  });
  it("nothing changed shows nothing", () => {
    expect(sinceLastVisit(record(), record({ at: T0 + 6 * DAY }))).toBeNull();
  });
  it("one line: when, then what changed", () => {
    const now = record({ at: T0 + 6 * DAY, moduleStart: T0 + DAY, config: "bbbb", module: "v2.8.2" });
    const line = sinceLastVisit(record(), now);
    expect(line?.text).toBe(
      "Since your last visit 6 d ago: the web server restarted or reloaded and the module changed from v2.8.1 to v2.8.2.",
    );
    expect(line?.iso).toBe(new Date(T0).toISOString());
    expect(sinceLastVisit(record(), record({ at: T0 + 3_600_000, config: "bbbb" }))?.text).toBe(
      "Since your last visit 1 h ago: the configuration changed.",
    );
  });
  it("a record from the future (clock moved back) shows nothing", () => {
    expect(sinceLastVisit(record({ at: T0 + DAY }), record({ moduleStart: T0 + 1 }))).toBeNull();
  });
});

describe("VisitOnce", () => {
  it("compares with the stored record once, then stores this visit", () => {
    const s = new MemoryStorage();
    writeVisit(s, "k", record());
    const v = new VisitOnce(s);
    expect(v.begin()).toBe(true);
    expect(v.begin()).toBe(false);
    const now = record({ at: T0 + DAY, moduleStart: T0 + 1 });
    expect(v.settle("k", now)?.text).toContain("the web server restarted or reloaded");
    expect(v.result?.text).toContain("the web server restarted or reloaded");
    expect(readVisit(s, "k")).toEqual(now);
  });
  it("the next page open compares with this one", () => {
    const s = new MemoryStorage();
    expect(new VisitOnce(s).settle("k", record())).toBeNull();
    expect(new VisitOnce(s).settle("k", record({ at: T0 + 1000 }))).toBeNull();
    expect(new VisitOnce(s).settle("k", record({ at: T0 + 2000, config: "bbbb" }))?.text).toContain("configuration changed");
  });
  it("a page mounted before the comparison gets its line when it is settled", async () => {
    const s = new MemoryStorage();
    writeVisit(s, "k", record());
    const v = new VisitOnce(s);
    expect(v.result).toBeNull();
    const waiting = v.settled;
    v.settle("k", record({ at: T0 + DAY, config: "bbbb" }));
    expect((await waiting)?.text).toBe("Since your last visit 1 d ago: the configuration changed.");
  });
  it("without storage: nothing to show, nothing thrown", () => {
    const v = new VisitOnce(new ThrowingStorage());
    expect(v.settle("k", record())).toBeNull();
    expect(new VisitOnce(null).settle("k", record())).toBeNull();
  });
});

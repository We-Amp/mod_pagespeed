// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  HostSet,
  LENS_STORAGE_KEY,
  LensFollower,
  hostsInText,
  lensFromParams,
  lensAddressRewrite,
  lensStorageKey,
  loadLens,
  normalizeLensHost,
  saveLens,
  textNamesHost,
  withLens,
} from "./host-lens";

function memoryStorage(initial: Record<string, string> = {}): Storage {
  const data = new Map(Object.entries(initial));
  return {
    get length() {
      return data.size;
    },
    clear: () => data.clear(),
    getItem: (k: string) => data.get(k) ?? null,
    key: (i: number) => [...data.keys()][i] ?? null,
    removeItem: (k: string) => void data.delete(k),
    setItem: (k: string, v: string) => void data.set(k, v),
  };
}

const throwing: Storage = {
  length: 0,
  clear: () => {
    throw new Error("blocked");
  },
  getItem: () => {
    throw new Error("blocked");
  },
  key: () => null,
  removeItem: () => {
    throw new Error("blocked");
  },
  setItem: () => {
    throw new Error("blocked");
  },
};

describe("normalizeLensHost", () => {
  // The optimizer's host-name rule, with the shapes its own tests use: a
  // row the optimizer reports is always a lens host, and a value it refuses
  // is never one.
  it("names a host the way the optimizer does: lowercase, no port, no trailing dot", () => {
    expect(normalizeLensHost("WWW.Example.Test")).toBe("www.example.test");
    expect(normalizeLensHost("www.example.com.")).toBe("www.example.com");
    expect(normalizeLensHost("localhost:81")).toBe("localhost");
    expect(normalizeLensHost("www.example.com.:443")).toBe("www.example.com");
  });

  it("accepts every name the optimizer reports: underscores, addresses, bracketed IPv6", () => {
    expect(normalizeLensHost("a_b.test")).toBe("a_b.test");
    expect(normalizeLensHost("10.1.2.3")).toBe("10.1.2.3");
    expect(normalizeLensHost("[::1]")).toBe("[::1]");
    expect(normalizeLensHost("[2001:db8::1]")).toBe("[2001:db8::1]");
    expect(normalizeLensHost("[2001:DB8::1]:80")).toBe("[2001:db8::1]");
    expect(normalizeLensHost("a".repeat(127))).toBe("a".repeat(127));
  });

  it("refuses what the optimizer refuses", () => {
    const refused: unknown[] = [
      "", "_", ".", "..", "-", "[:]", "[]", "www.example.com..",
      "a".repeat(128), "a".repeat(300),
      "[::1]x", "[zz::1]", "[::1", "[::1].", "a[b].test", "a]b.test",
      "::1", "2001:db8::1", "example.com:", "example.com:123456",
      " a.test", "a.test ", "bad host", "<img src=x>", 'a"b', "é.test", "a/b",
      "*.example.test", "(other)",
      123, null, undefined, {},
    ];
    for (const bad of refused) {
      expect(normalizeLensHost(bad), String(bad)).toBeNull();
    }
  });
});

describe("the lens in the hash", () => {
  it("reads lens= and ignores a malformed one", () => {
    expect(lensFromParams(new URLSearchParams("lens=CDN.example.test"))).toBe("cdn.example.test");
    expect(lensFromParams(new URLSearchParams("lens=%3Cimg%3E"))).toBeNull();
    expect(lensFromParams(new URLSearchParams(""))).toBeNull();
  });

  it("sets, replaces and removes lens= and keeps every other parameter", () => {
    expect(withLens("#/urls", "a.test")).toBe("#/urls?lens=a.test");
    expect(withLens("#/logs?level=error&lens=a.test", "b.test")).toBe("#/logs?level=error&lens=b.test");
    expect(withLens("#/logs?level=error&lens=a.test", null)).toBe("#/logs?level=error");
    expect(withLens("#/urls/detail?url=%2Fa&host=x.test&scheme=https", "y.test")).toBe(
      "#/urls/detail?url=%2Fa&host=x.test&scheme=https&lens=y.test",
    );
  });

  it("leaves every other parameter exactly as written: spelling, bare names and order", () => {
    // Added at the end.
    expect(withLens("#/logs?q=a%20b&t=~&flag", "a.test")).toBe("#/logs?q=a%20b&t=~&flag&lens=a.test");
    // Replaced where it stands: first, in the middle, last.
    expect(withLens("#/logs?lens=x.test&q=a%20b&flag", "a.test")).toBe("#/logs?lens=a.test&q=a%20b&flag");
    expect(withLens("#/logs?q=a%20b&lens=x.test&t=~", "a.test")).toBe("#/logs?q=a%20b&lens=a.test&t=~");
    expect(withLens("#/logs?q=a%20b&flag&lens=x.test", "a.test")).toBe("#/logs?q=a%20b&flag&lens=a.test");
    // Removed from wherever it stands.
    expect(withLens("#/logs?lens=x.test&q=a%20b", null)).toBe("#/logs?q=a%20b");
    expect(withLens("#/logs?q=a%20b&lens=x.test&flag", null)).toBe("#/logs?q=a%20b&flag");
    expect(withLens("#/logs?t=~&flag&lens=x.test", null)).toBe("#/logs?t=~&flag");
    // Absent and nothing to set: the address as it was.
    expect(withLens("#/logs?q=a%20b&t=~&flag", null)).toBe("#/logs?q=a%20b&t=~&flag");
    // An empty query.
    expect(withLens("#/urls?", "a.test")).toBe("#/urls?lens=a.test");
    expect(withLens("#/urls?", null)).toBe("#/urls");
    expect(withLens("#/urls?lens=a.test", null)).toBe("#/urls");
  });

  it("leaves exactly one lens= of several, or none when the lens is cleared", () => {
    expect(withLens("#/urls?lens=a.test&q=a%20b&lens=b.test", "c.test")).toBe("#/urls?lens=c.test&q=a%20b");
    expect(withLens("#/urls?lens=a.test&q=a%20b&lens=b.test", null)).toBe("#/urls?q=a%20b");
  });
});

describe("lensAddressRewrite", () => {
  it("adds the active lens to an address without one, keeping every other parameter", () => {
    expect(lensAddressRewrite(true, "#/statistics", "a.test")).toBe("#/statistics?lens=a.test");
    expect(lensAddressRewrite(true, "#/logs?level=warning&source=module", "a.test")).toBe(
      "#/logs?level=warning&source=module&lens=a.test",
    );
    expect(lensAddressRewrite(true, "", "a.test")).toBe("#/overview?lens=a.test");
  });

  it("replaces a malformed lens= with the active one", () => {
    expect(lensAddressRewrite(true, "#/urls?lens=%3Cimg%3E", "a.test")).toBe("#/urls?lens=a.test");
  });

  it("keeps the rest of a typed address as it was typed", () => {
    expect(lensAddressRewrite(true, "#/logs?q=a%20b&t=~&flag", "a.test")).toBe("#/logs?q=a%20b&t=~&flag&lens=a.test");
    expect(lensAddressRewrite(true, "#/logs?q=a%20b&lens=x.test&flag", "a.test")).toBe("#/logs?q=a%20b&lens=a.test&flag");
  });

  it("leaves an address with several lens= carrying exactly one, the active lens", () => {
    expect(lensAddressRewrite(true, "#/urls?lens=a.test&lens=b.test", "a.test")).toBe("#/urls?lens=a.test");
    expect(lensAddressRewrite(true, "#/urls?lens=A.TEST&q=1&lens=a.test", "a.test")).toBe("#/urls?lens=a.test&q=1");
  });

  it("leaves an address that already carries the active lens, in any spelling", () => {
    expect(lensAddressRewrite(true, "#/urls?lens=a.test", "a.test")).toBeNull();
    expect(lensAddressRewrite(true, "#/urls?lens=A.TEST", "a.test")).toBeNull();
  });

  it("never writes a lens when none is active, and never on a per-host console", () => {
    expect(lensAddressRewrite(true, "#/urls", null)).toBeNull();
    expect(lensAddressRewrite(true, "#/urls?lens=b.test", null)).toBeNull();
    expect(lensAddressRewrite(false, "#/urls", "a.test")).toBeNull();
    expect(lensAddressRewrite(false, "#/urls?lens=b.test", "a.test")).toBeNull();
  });

  it("carries an IPv6 site encoded, and a rewritten address needs no second rewrite", () => {
    const once = lensAddressRewrite(true, "#/savings", "[2001:db8::1]");
    expect(once).toBe("#/savings?lens=%5B2001%3Adb8%3A%3A1%5D");
    expect(lensAddressRewrite(true, once ?? "", "[2001:db8::1]")).toBeNull();
  });
});

describe("the lens in local storage", () => {
  it("round-trips a host and forgets it", () => {
    const s = memoryStorage();
    saveLens("a.test", s);
    expect(loadLens(s)).toBe("a.test");
    saveLens(null, s);
    expect(s.getItem(LENS_STORAGE_KEY)).toBeNull();
    expect(loadLens(s)).toBeNull();
  });

  it("tolerates a throwing or junk store", () => {
    expect(loadLens(throwing)).toBeNull();
    expect(() => saveLens("a.test", throwing)).not.toThrow();
    expect(loadLens(memoryStorage({ [LENS_STORAGE_KEY]: "<b>x</b>" }))).toBeNull();
    expect(loadLens(null)).toBeNull();
  });

  it("each console remembers its own lens: a per-host console cannot overwrite the whole-server one's", () => {
    const s = memoryStorage();
    const whole = lensStorageKey("/pagespeed_global_admin");
    const perHost = lensStorageKey("/pagespeed_admin");
    expect(whole).not.toBe(perHost);
    expect(whole.startsWith(`${LENS_STORAGE_KEY}:`)).toBe(true);
    saveLens("a.test", s, whole);
    saveLens("b.test", s, perHost);
    expect(loadLens(s, whole)).toBe("a.test");
    expect(loadLens(s, perHost)).toBe("b.test");
  });
});

describe("LensFollower", () => {
  it("a console found to be whole-server only after it started restores the remembered lens, once", () => {
    // The admin path did not say whole-server, so nothing was restored at start.
    const f = new LensFollower(false, () => "a.test");
    expect(f.next(false, "#/urls", null)).toBeUndefined();
    expect(f.next(true, "#/urls", null)).toBe("a.test");
    // Once: clearing it afterwards keeps it cleared.
    expect(f.next(true, "#/statistics", null)).toBeUndefined();
  });

  it("never over a lens= in the address or a lens already selected", () => {
    const linked = new LensFollower(false, () => "a.test");
    expect(linked.next(true, "#/urls?lens=b.test", null)).toBe("b.test");
    expect(linked.next(true, "#/urls", null)).toBeUndefined();
    const selected = new LensFollower(false, () => "a.test");
    expect(selected.next(true, "#/urls", "c.test")).toBeUndefined();
    expect(selected.next(true, "#/urls", null)).toBeUndefined();
  });

  it("a lens= the viewer has since cleared from the address is not selected again", () => {
    // Restored at start; the viewer opened a link with lens=, then cleared it,
    // which rewrote the address in place. A later scope answer reads the live
    // address, which no longer carries it.
    const f = new LensFollower(true, () => "a.test");
    expect(f.next(true, "#/urls?lens=b.test", null)).toBe("b.test");
    expect(f.next(true, "#/urls", null)).toBeUndefined();
  });

  it("a per-host console never applies one; a malformed lens= is ignored", () => {
    const f = new LensFollower(false, () => "a.test");
    expect(f.next(false, "#/urls?lens=b.test", null)).toBeUndefined();
    expect(new LensFollower(true, () => null).next(true, "#/urls?lens=%3Cb%3E", null)).toBeUndefined();
  });
});

describe("hosts in log text", () => {
  it("names the hosts of the http(s) URLs a text links, once each, lowercased", () => {
    expect(hostsInText("see https://WWW.A.test/x and http://b.test:8080/y, javascript:alert(1), https://www.a.test/z")).toEqual([
      "www.a.test",
      "b.test",
    ]);
  });

  it("a quote ends the URL, so markup after it is never part of a host", () => {
    expect(hostsInText('https://evil.test"><img src=x onerror=alert(1)>/')).toEqual(["evil.test"]);
  });

  it("compares hosts exactly: a parent domain is not its subdomain", () => {
    expect(textNamesHost("fetch of https://www.example.test/a failed", "www.example.test")).toBe(true);
    expect(textNamesHost("fetch of https://www.example.test/a failed", "example.test")).toBe(false);
    expect(textNamesHost("no URL here, example.test", "example.test")).toBe(false);
  });
});

describe("HostSet", () => {
  it("lists hosts sorted, once each, ignoring anything outside the grammar", () => {
    const set = new HostSet();
    expect(set.add(["b.test", "A.test", "<x>", "b.test"], 1)).toBe(true);
    expect(set.list()).toEqual(["a.test", "b.test"]);
    expect(set.add(["a.test"], 0)).toBe(false);
  });

  it("keeps at most 100 hosts, served hosts first", () => {
    const set = new HostSet(3);
    set.add(["l1.test", "l2.test", "l3.test"], 2);
    expect(set.add(["served.test"], 0)).toBe(true);
    expect(set.list()).toContain("served.test");
    expect(set.list()).toHaveLength(3);
    const full = new HostSet(2);
    full.add(["a.test", "b.test"], 0);
    expect(full.add(["log.test"], 2)).toBe(false);
    expect(full.list()).toEqual(["a.test", "b.test"]);
    expect(new HostSet().cap).toBe(100);
  });
});

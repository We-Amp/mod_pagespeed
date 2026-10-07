// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { DaemonCooldownEntry } from "$lib/api/types";
import {
  DEFAULT_URL_SORT,
  buildUrlRows,
  detailHref,
  displayUrl,
  entryKey,
  entryKeyFromAbsoluteUrl,
  globalConsoleHref,
  globalConsoleUrlsHref,
  isValidEntryPath,
  nextUrlSortState,
  pageWindow,
  parseDetailKey,
} from "./urls-api";

// The daemon's shape: `url` is the recorded path + query, never absolute.
const URLS_BODY = {
  urls: [
    { url: "/", hostname: "www.example.test", scheme: "https", alternate_count: 3 },
    { url: "/hero.png", hostname: "www.example.test", scheme: "https", alternate_count: 2 },
    { url: "/app.js?v=2", hostname: "cdn.example.test", scheme: "http", alternate_count: 0 },
  ],
  offset: 0,
  limit: 50,
  next_offset: 3,
  has_more: false,
  total: 3,
};

const COOLDOWNS: DaemonCooldownEntry[] = [
  { url: "/hero.png", hostname: "www.example.test", scheme: "https", reason: "processing", remaining_seconds: 42 },
  // Same path, another host: must NOT match the hero row.
  { url: "/", hostname: "other.example.test", scheme: "https", reason: "write_failure" },
];

const HERO = { url: "/hero.png", host: "www.example.test", scheme: "https" } as const;

describe("isValidEntryPath (the module's url rule)", () => {
  it("accepts recorded paths with a query", () => {
    for (const ok of ["/", "/hero.png", "/a%20b.png?x=1&y=2", "/a%25zz", "/a+b"]) {
      expect(isValidEntryPath(ok), ok).toBe(true);
    }
  });
  it("rejects absolute URLs, relative paths, spaces, controls, non-ASCII, fragments", () => {
    for (const bad of [
      "", "hero.png", "https://www.example.test/hero.png", "/a b", "/a\u0000",
      "/a\u007f", "/café", "/a#f", `/${"a".repeat(2048)}`,
    ]) {
      expect(isValidEntryPath(bad), JSON.stringify(bad)).toBe(false);
    }
    expect(isValidEntryPath(`/${"a".repeat(2047)}`)).toBe(true);
  });
});

describe("entryKey", () => {
  it("builds a key from valid parts only", () => {
    expect(entryKey("/hero.png", "www.example.test", "https")).toEqual(HERO);
    expect(entryKey("/hero.png", "[::1]", "http")).toEqual({ url: "/hero.png", host: "[::1]", scheme: "http" });
    expect(entryKey("/hero.png", "", "https")).toBeNull();
    expect(entryKey("/hero.png", "bad host", "https")).toBeNull();
    expect(entryKey("/hero.png", "www.example.test", "ftp")).toBeNull();
    expect(entryKey(7, "www.example.test", "https")).toBeNull();
  });
});

describe("detailHref / parseDetailKey", () => {
  it("round-trips path + query, host and scheme through the hash query", () => {
    const key = { url: "/a%20b.png?x=1&y=2", host: "www.example.test", scheme: "https" } as const;
    const href = detailHref(key);
    expect(href.startsWith("#/urls/detail?")).toBe(true);
    const params = new URLSearchParams(href.split("?").slice(1).join("?"));
    expect(parseDetailKey(params)).toEqual(key);
  });
  it("rejects a missing part, an absolute url, or bad values", () => {
    expect(parseDetailKey(new URLSearchParams())).toBeNull();
    expect(parseDetailKey(new URLSearchParams("url=%2Fhero.png&host=www.example.test"))).toBeNull();
    expect(parseDetailKey(new URLSearchParams("url=%2Fhero.png&scheme=https"))).toBeNull();
    expect(
      parseDetailKey(new URLSearchParams("url=https%3A%2F%2Fx.test%2Fa&host=x.test&scheme=https")),
    ).toBeNull();
    expect(parseDetailKey(new URLSearchParams("url=%2Fa%23b&host=x.test&scheme=https"))).toBeNull();
    expect(parseDetailKey(new URLSearchParams("url=%2Fa&host=x.test&scheme=ftp"))).toBeNull();
  });
});

describe("entryKeyFromAbsoluteUrl", () => {
  it("splits a full URL into path + query, host and scheme", () => {
    expect(entryKeyFromAbsoluteUrl("https://www.example.test/hero.png?v=2")).toEqual({
      url: "/hero.png?v=2",
      host: "www.example.test",
      scheme: "https",
    });
    // The URL API canonicalizes like the module does: lowercase host, no
    // port, percent-encoded path, "/" for an empty path.
    expect(entryKeyFromAbsoluteUrl("HTTP://WWW.Example.TEST:8080/a b.png")).toEqual({
      url: "/a%20b.png",
      host: "www.example.test",
      scheme: "http",
    });
    expect(entryKeyFromAbsoluteUrl("https://www.example.test")).toEqual({
      url: "/",
      host: "www.example.test",
      scheme: "https",
    });
  });
  it("is null for anything that is not an absolute http(s) URL", () => {
    expect(entryKeyFromAbsoluteUrl("/relative")).toBeNull();
    expect(entryKeyFromAbsoluteUrl("ftp://x.test/a")).toBeNull();
    expect(entryKeyFromAbsoluteUrl("not a url")).toBeNull();
  });
});

describe("displayUrl", () => {
  it("joins scheme, host and path + query", () => {
    expect(displayUrl(HERO)).toBe("https://www.example.test/hero.png");
  });
});

describe("globalConsoleUrlsHref", () => {
  it("maps the per-vhost admin path to the whole-server URLs page", () => {
    expect(globalConsoleUrlsHref("/pagespeed_admin")).toBe(
      "/pagespeed_global_admin/#/urls",
    );
  });
  it("is null on the global console itself and on a renamed admin path", () => {
    expect(globalConsoleUrlsHref("/pagespeed_global_admin")).toBeNull();
    expect(globalConsoleUrlsHref("/renamed_admin")).toBeNull();
  });
});

describe("globalConsoleHref", () => {
  it("maps any mount ending in the per-vhost admin path to the whole-server page", () => {
    expect(globalConsoleHref("/pagespeed_admin", "#/logs")).toBe("/pagespeed_global_admin/#/logs");
    expect(globalConsoleHref("/site/pagespeed_admin", "#/urls")).toBe("/site/pagespeed_global_admin/#/urls");
  });
  it("is null on the global console itself and on a renamed admin path", () => {
    expect(globalConsoleHref("/pagespeed_global_admin", "#/logs")).toBeNull();
    expect(globalConsoleHref("/renamed_admin", "#/logs")).toBeNull();
  });
});

describe("buildUrlRows", () => {
  it("maps entries to keys and joins cooldowns on the full key", () => {
    const rows = buildUrlRows(URLS_BODY, COOLDOWNS, "", DEFAULT_URL_SORT);
    expect(rows).toHaveLength(3);
    const hero = rows.find((r) => r.display === "https://www.example.test/hero.png");
    expect(hero?.key).toEqual(HERO);
    expect(hero?.variants).toBe(2);
    expect(hero?.cooldown?.reason).toBe("processing");
    const home = rows.find((r) => r.display === "https://www.example.test/");
    expect(home?.cooldown).toBeNull(); // the "/" cooldown belongs to another host
    expect(rows.find((r) => r.host === "cdn.example.test")?.display).toBe(
      "http://cdn.example.test/app.js?v=2",
    );
  });
  it("under a lens, only that host's rows, compared exactly", () => {
    const rows = buildUrlRows(URLS_BODY, COOLDOWNS, "", DEFAULT_URL_SORT, "www.example.test");
    expect(rows.map((r) => r.host)).toEqual(["www.example.test", "www.example.test"]);
    expect(buildUrlRows(URLS_BODY, COOLDOWNS, "", DEFAULT_URL_SORT, "example.test")).toEqual([]);
    expect(buildUrlRows(URLS_BODY, COOLDOWNS, "", DEFAULT_URL_SORT, null)).toHaveLength(3);
  });
  it("survives a garbage reply without NaN or undefined leaking", () => {
    const garbage = {
      urls: [
        null,
        42,
        {},
        { url: 7 },
        { url: "" },
        { url: "/ok.png", hostname: "ok.test", scheme: "https", alternate_count: "many" },
        { url: "/neg.png", hostname: "neg.test", scheme: "https", alternate_count: -2 },
        { url: "/nohost.png", scheme: "https", alternate_count: 1 },
      ],
      total: -3,
      has_more: "yes",
    };
    const rows = buildUrlRows(garbage as never, [], "", DEFAULT_URL_SORT);
    expect(rows.map((r) => r.display)).toEqual([
      "https:///nohost.png",
      "https://neg.test/neg.png",
      "https://ok.test/ok.png",
    ]);
    for (const row of rows) {
      expect(Number.isNaN(row.variants)).toBe(false);
      expect(row.variants).toBeGreaterThanOrEqual(0);
      expect(row.cooldown).toBeNull();
    }
    // A row without a usable host cannot be opened.
    expect(rows.find((r) => r.display.includes("nohost"))?.key).toBeNull();
    const win = pageWindow(garbage as never, 0, rows.length);
    expect(win.total).toBeNull(); // -3 is not a sane total
    expect(win.hasMore).toBe(false); // "yes" is not true
  });
  it("keys duplicate entries distinctly: the each-block keys can never collide", () => {
    const dup = { url: "/dup.png", hostname: "a.test", scheme: "https", alternate_count: 1 };
    // Two identical entries, and two unusable entries that render alike.
    const rows = buildUrlRows(
      { urls: [dup, dup, { url: "/x", hostname: "h.test", scheme: "gopher" }, { url: "/x", hostname: "h.test", scheme: "ftp" }] } as never,
      [],
      "",
      DEFAULT_URL_SORT,
    );
    expect(rows).toHaveLength(4);
    expect(new Set(rows.map((r) => r.rowKey)).size).toBe(4);
  });
  it("filters on the full URL and the host, case-insensitively", () => {
    expect(buildUrlRows(URLS_BODY, [], "HERO", DEFAULT_URL_SORT).map((r) => r.display)).toEqual([
      "https://www.example.test/hero.png",
    ]);
    expect(buildUrlRows(URLS_BODY, [], "cdn", DEFAULT_URL_SORT).map((r) => r.display)).toEqual([
      "http://cdn.example.test/app.js?v=2",
    ]);
  });
  it("sorts by url, host and variants; variant ties stay url-ordered", () => {
    const byHost = buildUrlRows(URLS_BODY, [], "", { key: "host", asc: true });
    expect(byHost[0].host).toBe("cdn.example.test");
    const byVariants = buildUrlRows(URLS_BODY, [], "", { key: "variants", asc: false });
    expect(byVariants.map((r) => r.variants)).toEqual([3, 2, 0]);
    const byVariantsAsc = buildUrlRows(URLS_BODY, [], "", { key: "variants", asc: true });
    expect(byVariantsAsc[0].display).toBe("http://cdn.example.test/app.js?v=2");
  });
});

describe("nextUrlSortState", () => {
  it("flips the direction on the active key, else starts natural", () => {
    expect(nextUrlSortState(DEFAULT_URL_SORT, "url")).toEqual({ key: "url", asc: false });
    expect(nextUrlSortState(DEFAULT_URL_SORT, "host")).toEqual({ key: "host", asc: true });
    expect(nextUrlSortState(DEFAULT_URL_SORT, "variants")).toEqual({ key: "variants", asc: false });
  });
});

describe("pageWindow", () => {
  it("describes a full first page", () => {
    expect(pageWindow(URLS_BODY, 0, 3)).toEqual({ from: 1, to: 3, total: 3, hasMore: false });
  });
  it("describes an empty index and a missing body", () => {
    expect(pageWindow({ urls: [], total: 0, has_more: false }, 0, 0)).toEqual({
      from: 0,
      to: 0,
      total: 0,
      hasMore: false,
    });
    expect(pageWindow(undefined, 0, 0).total).toBeNull();
  });
});

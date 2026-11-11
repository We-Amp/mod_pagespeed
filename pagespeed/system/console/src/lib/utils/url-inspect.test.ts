// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ApiError, UnavailableError } from "$lib/api/client";
import { WEBSITE } from "$lib/data/product-facts-console";
import type { CacheEntryResponse, DaemonCooldownEntry } from "$lib/api/types";
import {
  INSPECT_INPUT_HELP,
  cooldownFor,
  cooldownText,
  inspectHrefForLink,
  inspectTarget,
  logsHrefFor,
  lookupIsFinal,
  moduleLookup,
  urlReasons,
  urlVerdict,
  wayBack,
  type ModuleLookup,
  type UrlVerdictInput,
  wholeServerLine,
} from "./url-inspect";
import { toVariantRows } from "./url-detail";
import { computeUrlStatus } from "./url-status";

const KEY = { url: "/img/hero.png?v=2", host: "www.example.test", scheme: "https" } as const;
const fulfilled = <T>(value: T): PromiseSettledResult<T> => ({ status: "fulfilled", value });
const rejected = <T>(reason: unknown): PromiseSettledResult<T> => ({ status: "rejected", reason });

describe("the pasted URL", () => {
  it("a full URL becomes its inspect page: path and query, host without port, scheme", () => {
    const target = inspectTarget("  https://WWW.Example.test:8443/img/hero.png?v=2#top \n");
    expect(target).toEqual({
      ok: true,
      key: KEY,
      href: "#/urls/detail?url=%2Fimg%2Fhero.png%3Fv%3D2&host=www.example.test&scheme=https",
    });
  });

  it("a bare host is the site's root", () => {
    const target = inspectTarget("http://site-a.test");
    expect(target.ok && target.key).toEqual({ url: "/", host: "site-a.test", scheme: "http" });
  });

  it("anything that is not a full http(s) URL is refused with what is expected, never guessed", () => {
    for (const text of ["", "   ", "/img/hero.png", "www.example.test/hero.png", "ftp://example.test/a", "javascript:alert(1)", "https://"]) {
      expect(inspectTarget(text), text).toEqual({ ok: false, message: INSPECT_INPUT_HELP });
    }
  });

  it("links to the log entries that name the URL, at every level", () => {
    expect(logsHrefFor(KEY, "module")).toBe(
      "#/logs?source=module&level=debug&q=https%3A%2F%2Fwww.example.test%2Fimg%2Fhero.png%3Fv%3D2",
    );
    expect(logsHrefFor(KEY, "optimizer")).toContain("source=optimizer");
  });

  it("a URL in a log line has an inspect page; anything else has none", () => {
    expect(inspectHrefForLink("https://www.example.test/a.css")).toBe(
      "#/urls/detail?url=%2Fa.css&host=www.example.test&scheme=https",
    );
    expect(inspectHrefForLink("mailto:someone@example.test")).toBeNull();
    // The product's own website (the documentation link in a message) is never the operator's URL.
    expect(inspectHrefForLink(`${WEBSITE}/docs/admin-console/#access-control`)).toBeNull();
    expect(inspectHrefForLink("not a url")).toBeNull();
  });
});

describe("the module's half", () => {
  const dump = (ok: boolean) =>
    `Metadata cache key:rname/ic_abc/https://www.example.test/x.png\ncache_ok:${ok}\ncan_revalidate:false\npartitions is NULL\n`;

  it("reads the module's lookup answer", () => {
    expect(moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value: dump(true) })).kind).toBe("current");
    expect(moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value: dump(false) })).kind).toBe("not-current");
    const original = moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value: "Unable to decode resource." }));
    expect(original.kind).toBe("original-url");
    expect(original.text).toContain(".pagespeed.");
  });

  it("an answer without a result is said, never read as a result", () => {
    expect(moduleLookup(undefined).kind).toBe("no-answer");
    expect(moduleLookup(fulfilled({ caches: [] } as unknown as CacheEntryResponse)).kind).toBe("no-answer");
    expect(moduleLookup(fulfilled(null as unknown as CacheEntryResponse)).kind).toBe("no-answer");
    for (const value of ["something new", "Invalid URL", ""]) {
      expect(moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value })).kind, value).toBe("no-answer");
    }
    // A failed or busy read has no result either, and no transport text reaches the sentence.
    for (const reason of [new ApiError(500, "boom"), new ApiError(429, "busy"), new Error("network")]) {
      const view = moduleLookup(rejected(reason));
      expect(view.kind).toBe("no-answer");
      expect(view.text).not.toMatch(/HTTP|boom|busy|network/);
    }
  });

  it("names a refusal", () => {
    expect(moduleLookup(rejected(new ApiError(403, "URL belongs to another host; use that host's console"))).kind).toBe("other-host");
    // A lookup that is turned off has no result; the page says so without a transport error.
    expect(moduleLookup(rejected(new UnavailableError(403, "Forbidden", "/cache?url"))).kind).toBe("no-answer");
  });

  it("an answer that cannot change is final; the others are asked again", () => {
    expect(["current", "not-current", "original-url", "other-host", "no-answer"].filter((k) => lookupIsFinal(k as never))).toEqual([
      "original-url",
      "other-host",
    ]);
  });
});

describe("the optimizer's half", () => {
  const COOLDOWNS: DaemonCooldownEntry[] = [
    { url: "/img/hero.png?v=2", hostname: "other.example.test", scheme: "https", reason: "write_failure" },
    { url: "/img/hero.png?v=2", hostname: "www.example.test", scheme: "http", reason: "revalidation" },
    { url: "/img/hero.png?v=2", hostname: "www.example.test", scheme: "https", reason: "processing", remaining_seconds: 41.6 },
  ];

  it("finds the cooldown of exactly this URL: path, host and scheme", () => {
    expect(cooldownFor(COOLDOWNS, KEY)).toBe(COOLDOWNS[2]);
    expect(cooldownFor(COOLDOWNS, { ...KEY, url: "/img/hero.png" })).toBeNull();
    expect(cooldownFor([], KEY)).toBeNull();
  });

  it("says a cooldown with what it knows, and nothing it does not", () => {
    expect(cooldownText(COOLDOWNS[2])).toBe("In cooldown (Processing), 42 s remaining.");
    expect(cooldownText({ reason: "made_up", remaining_seconds: Number.NaN })).toBe("In cooldown.");
    expect(cooldownText({ remaining_seconds: -3 })).toBe("In cooldown, 0 s remaining.");
  });

  it("says a long cooldown in minutes or hours, as the console's other sentences do", () => {
    expect(cooldownText({ reason: "write_failure", remaining_seconds: 125 })).toBe("In cooldown (Write failed), 2 min remaining.");
    expect(cooldownText({ remaining_seconds: 7300 })).toBe("In cooldown, 2 h remaining.");
  });

  it("a page without the optimizer's record says what the whole-server console adds, or that there is no optimizer", () => {
    expect(wholeServerLine(false)).toEqual({ text: "The whole-server console adds the optimizer's record of this URL.", link: true });
    expect(wholeServerLine(true)).toEqual({
      text: "No optimizer is configured for this server; the module optimizes on its own. The module's answer is the whole answer.",
      link: false,
    });
  });
});

describe("the reasons", () => {
  it("lists every usual reason once, each with a documentation link on the product site", () => {
    const reasons = urlReasons(null);
    expect(reasons.map((r) => r.id)).toEqual([
      "not-requested",
      "not-cacheable",
      "host-not-authorized",
      "original-kept",
    ]);
    for (const reason of reasons) {
      expect(reason.evidence, reason.id).toBeNull();
      expect(reason.title, reason.id).not.toBe("");
      expect(reason.text, reason.id).toMatch(/\.$/);
      expect(reason.doc.label, reason.id).not.toBe("");
      expect(reason.doc.href, reason.id).toMatch(/^https:\/\/[a-z.]+\/docs\/[a-z-]+\/(#[a-z0-9-]+)?$/);
    }
    expect(new Set(reasons.map((r) => r.doc.href)).size).toBe(reasons.length);
  });

  it("a cooldown is evidence the URL was tried: that reason comes first and says so", () => {
    const reasons = urlReasons({ reason: "processing", remaining_seconds: 42 });
    expect(reasons[0]).toMatchObject({ id: "original-kept", evidence: "In cooldown (Processing), 42 s remaining." });
    expect(reasons).toHaveLength(4);
    expect(reasons.slice(1).every((r) => r.evidence === null)).toBe(true);
  });
});

describe("the verdict", () => {
  const variant = (over: Record<string, unknown>) => ({
    is_sentinel: false, format: "original", encoding: "identity", content_type: "image", hit_count: 0, ...over,
  });
  const record = (alternates: Record<string, unknown>[]) => {
    const rows = toVariantRows({ alternates });
    return { rows, status: computeUrlStatus(rows.map((r) => r.raw)) };
  };
  const look = (kind: Parameters<typeof moduleLookup>[0] | ModuleLookup) =>
    kind !== undefined && "kind" in kind ? kind : moduleLookup(kind);
  const input = (over: Partial<Omit<UrlVerdictInput, "lookup">> & { lookup?: ModuleLookup }): UrlVerdictInput => ({
    record: null, notInIndex: false, access: "read", cooldown: null, lookup: look(undefined), ...over,
  });
  const line = (over: Parameters<typeof input>[0]) => {
    const v = urlVerdict(input(over));
    return [v.kind, v.word + v.text];
  };
  const dump = (partitions: string) =>
    moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value: `Metadata cache key:k\ncache_ok:true\ncan_revalidate:false\n${partitions}\n` }));
  const OPTIMIZED = dump('partitions:partition {\n  url: "https://www.example.test/a.css.pagespeed.cf.x.css"\n  input {\n    url: "https://www.example.test/a.css"\n  }\n}\n');
  const KEPT = dump('partitions:partition {\n  optimizable: false\n  input {\n    url: "https://www.example.test/a.css"\n  }\n}\n');
  const SILENT = dump("partitions is NULL");
  const NOT_CURRENT = moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value: "cache_ok:false\n" }));
  const ORIGINAL = moduleLookup(fulfilled<CacheEntryResponse>({ url: "", value: "Unable to decode resource." }));
  const OTHER_HOST = moduleLookup(rejected(new ApiError(403, "HTTP 403: URL belongs to another host; use that host's console")));

  it("reads what a current module result did, never an input's url", () => {
    expect(OPTIMIZED.outcome).toBe("optimized");
    expect(KEPT.outcome).toBe("kept");
    expect(SILENT.outcome).toBeNull();
    expect(KEPT.kind).toBe("current");
  });

  it("a URL that spells cache_ok:true does not make a stale entry current", () => {
    const stale = moduleLookup(
      fulfilled<CacheEntryResponse>({
        url: "",
        value: 'Metadata cache key:rname/cf/https://www.example.test/x?cache_ok:true@_\ncache_ok:false\ncan_revalidate:false\npartitions:partition {\n  url: "https://www.example.test/x.pagespeed.cf.a.css"\n}\n',
      }),
    );
    expect(stale.kind).toBe("not-current");
    expect(line({ access: "none", lookup: stale })[0]).toBe("not-yet");
  });

  it("a variant whose metadata could not be read is no evidence either way", () => {
    const fallback = record([variant({ size: 45210, flags: 0, content_type: "" })]);
    expect(line({ record: fallback })).toEqual(["unknown", "Unknown. The optimizer's record could not be read."]);
  });

  it("a worker variant that is no smaller names no saving", () => {
    const same = record([variant({ format: "jpeg", flags: 2, size: 45210, original_size: 45210, hit_count: 4 })]);
    expect(line({ record: same })).toEqual(["optimized", "Optimized."]);
  });

  it("an optimized URL names what the most-served variant saves and how it looks", () => {
    const hero = record([
      variant({ size: 45210, original_size: 45210, hit_count: 128 }),
      variant({ format: "webp", size: 30984, original_size: 45210, hit_count: 1042, ssimulacra2_score: 92.5 }),
      variant({ format: "avif", size: 28120, original_size: 45210, hit_count: 88, ssimulacra2_score: 87.1 }),
    ]);
    expect(line({ record: hero })).toEqual([
      "optimized",
      "Optimized. Browsers that accept WebP get 30.3 KB instead of 44.2 KB (-31.5%). Visual score 92.5, good quality.",
    ]);
  });

  it("the optimizer's real shape: the original is a sentinel, one variant alone is optimized", () => {
    const real = record([
      { alternate_id: 12, size: 45210, is_sentinel: true, sentinel_name: "original_content" },
      variant({ format: "webp", size: 30984, original_size: 45210, hit_count: 13, ssimulacra2_score: 60 }),
    ]);
    expect(line({ record: real })[1]).toBe(
      "Optimized. Browsers that accept WebP get 30.3 KB instead of 44.2 KB (-31.5%). Visual score 60.0, acceptable quality.",
    );
  });

  it("a re-encode in the origin's own format is optimized, and goes to every browser", () => {
    const jpeg = record([
      variant({ size: 45210, original_size: 45210, flags: 0, hit_count: 3 }),
      variant({ format: "jpeg", flags: 2, size: 30984, original_size: 45210, hit_count: 9 }),
    ]);
    expect(jpeg.status).toBe("partial");
    expect(line({ record: jpeg })[1]).toBe("Optimized. Browsers get 30.3 KB instead of 44.2 KB (-31.5%).");
  });

  it("a compressed text variant names its encoding; without sizes the word stands alone", () => {
    const css = record([
      variant({ content_type: "css", size: 9000, original_size: 9000 }),
      variant({ content_type: "css", encoding: "brotli", size: 2000, original_size: 9000, hit_count: 5 }),
    ]);
    expect(line({ record: css })[1]).toBe("Optimized. Browsers that accept brotli get 1.95 KB instead of 8.79 KB (-77.8%).");
    expect(line({ record: record([variant({ format: "avif" }), variant({ format: "webp" })]) })[1]).toBe("Optimized.");
  });

  it("the module's result is optimized only when it says so", () => {
    expect(line({ access: "not-here", lookup: OPTIMIZED })).toEqual([
      "optimized",
      "Optimized. The module holds an optimized result for it.",
    ]);
    expect(line({ access: "none", lookup: KEPT })).toEqual(["kept", "Not optimized. The module tried it and kept the original."]);
    expect(line({ access: "none", lookup: SILENT })).toEqual([
      "unknown",
      "Unknown. The module's result does not say.",
    ]);
  });

  it("a cooldown says why the URL waits, in the console's time words", () => {
    expect(line({ notInIndex: true, cooldown: { reason: "processing", remaining_seconds: 125 } })).toEqual([
      "not-yet",
      "Not optimized yet. In cooldown (Processing), 2 min remaining.",
    ]);
  });


  it("not yet: only when the optimizer's record was read, or there is no optimizer", () => {
    expect(line({ record: record([variant({ size: 1 }), variant({ size: 2 })]) })).toEqual([
      "not-yet",
      "Not optimized yet. The optimizer holds no optimized variant of it.",
    ]);
    expect(line({ record: record([{ is_sentinel: true, sentinel_name: "content_hash" }]) })[1]).toBe(
      "Not optimized yet. The optimizer holds no optimized variant of it.",
    );
    expect(line({ notInIndex: true, lookup: ORIGINAL })[1]).toBe("Not optimized yet. The optimizer has no record of it.");
    expect(line({ access: "none", lookup: NOT_CURRENT })[1]).toBe(
      "Not optimized yet. The module holds no current rewrite result for this URL.",
    );
  });

  it("unknown always says why: the record out of reach here, or what the module could not say", () => {
    expect(line({ access: "not-here", lookup: NOT_CURRENT })).toEqual([
      "unknown",
      "Unknown. The optimizer's record is on the whole-server console.",
    ]);
    expect(line({ access: "not-answering", lookup: NOT_CURRENT })[1]).toBe("Unknown. The optimizer is not answering.");
    expect(line({ access: "unreadable", lookup: ORIGINAL })[1]).toBe("Unknown. The optimizer's record could not be read.");
    expect(line({ access: "none", lookup: ORIGINAL })[1]).toBe(
      "Unknown. The module's lookup answers only for rewritten URLs, and no optimizer is configured.",
    );
    expect(line({ access: "none", lookup: OTHER_HOST })[1]).toBe(
      "Unknown. This URL belongs to another host; that host's console answers for it.",
    );
    expect(line({ access: "none" })[1]).toBe("Unknown. The module gave no lookup result.");
  });
});

describe("the way back", () => {
  it("returns to the page the reader came from, with its view", () => {
    expect(wayBack("#/urls?offset=50&q=hero", true)).toEqual({ href: "#/urls?offset=50&q=hero", label: "URLs" });
    expect(wayBack("#/logs?source=module&level=debug&q=x", false)).toEqual({ href: "#/logs?source=module&level=debug&q=x", label: "Logs" });
    expect(wayBack("#/overview", false)).toEqual({ href: "#/overview", label: "Overview" });
    expect(wayBack("#/optimizer", true)).toEqual({ href: "#/optimizer", label: "Optimizer status" });
  });

  it("opened directly, from another URL's page or an unknown address: the page it lives under on this console", () => {
    for (const previous of ["", "#/urls/detail?url=%2Fa&host=a.test&scheme=https", "#/nowhere"]) {
      expect(wayBack(previous, true)).toEqual({ href: "#/urls", label: "URLs" });
      expect(wayBack(previous, false)).toEqual({ href: "#/overview", label: "Overview" });
    }
  });
});

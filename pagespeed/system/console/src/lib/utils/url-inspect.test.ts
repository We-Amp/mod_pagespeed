// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ApiError, UnavailableError } from "$lib/api/client";
import type { CacheEntryResponse, DaemonCooldownEntry, MessagesResponse } from "$lib/api/types";
import {
  INSPECT_INPUT_HELP,
  cooldownFor,
  cooldownText,
  inspectHrefForLink,
  inspectTarget,
  logMentions,
  logMentionsText,
  logsHrefFor,
  moduleLookup,
  urlReasons,
  wholeServerLine,
} from "./url-inspect";

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

  const MESSAGES: MessagesResponse = {
    messages: [
      { timestamp: 0, severity: "error", message: "Fetch of https://www.example.test/img/hero.png?v=2 failed" },
      { timestamp: 0, severity: "info", message: "Rewrote HTTPS://WWW.EXAMPLE.TEST/img/hero.png?v=2.pagespeed.ic.x.png" },
      { timestamp: 0, severity: "info", message: "Slow origin for https://www.example.test/other.png" },
      { timestamp: 0, severity: "info", message: 7 as unknown as string },
    ],
  };

  it("counts the log entries that name the URL, as the Logs filter matches them", () => {
    const shown = "https://www.example.test/img/hero.png?v=2";
    expect(logMentions(fulfilled(MESSAGES), shown)).toBe(2);
    expect(logMentions(fulfilled({ messages: [] }), shown)).toBe(0);
    expect(logMentions(fulfilled({ messages: "no" } as unknown as MessagesResponse), shown)).toBe(0);
  });

  it("a log that could not be read has no count", () => {
    expect(logMentions(rejected(new UnavailableError(403, "Forbidden", "/message_history")), "x")).toBeNull();
    expect(logMentions(rejected(new ApiError(500, "boom")), "x")).toBeNull();
    expect(logMentions(undefined, "x")).toBeNull();
  });

  it("says the count in words", () => {
    expect(logMentionsText(0)).toBe("No entry in the module's log names this URL.");
    expect(logMentionsText(1)).toBe("1 entry in the module's log names this URL.");
    expect(logMentionsText(12)).toBe("12 entries in the module's log name this URL.");
    expect(logMentionsText(null)).toBe("The module's log could not be read.");
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

  it("a console without the optimizer's record says what the whole-server console adds", () => {
    expect(wholeServerLine(null)).toMatchObject({ link: true });
    expect(wholeServerLine("not-answering")).toMatchObject({ link: true });
    expect(wholeServerLine(null).text).toMatch(/whole-server console adds/);
  });

  it("a server without an optimizer has nothing more to link to", () => {
    for (const absence of ["not-configured", "module-unsupported"] as const) {
      const line = wholeServerLine(absence);
      expect(line.link).toBe(false);
      expect(line.text).toMatch(/No optimizer/);
    }
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

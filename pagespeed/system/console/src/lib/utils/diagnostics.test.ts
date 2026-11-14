// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { ApiError } from "$lib/api/client";
import { NetworkError } from "$lib/api/connection";
import type { DaemonHealthResponse } from "$lib/api/types";
import {
  cacheOf,
  collectDiagnostics,
  configHash,
  copyText,
  diagnosticsFileName,
  diagnosticsText,
  healthChecks,
  KEY_COUNTERS,
  lastWarnings,
  maskClientAddresses,
  optimizerLine,
  redactConfig,
  scrubSecrets,
  stripQueryStrings,
  type DiagnosticsApi,
  type DiagnosticsData,
} from "./diagnostics";

const AT = Date.UTC(2026, 9, 2, 10, 25, 51);
const EMPTY_SHA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const msg = (severity: string, message: string) => ({ severity, message });
const WHOLE = "whole server";

/** A bundle's inputs with nothing remarkable in them; tests override what they look at. */
const data = (over: Partial<DiagnosticsData> = {}): DiagnosticsData => ({
  generatedAt: AT,
  build: "v1.16.0-86-g0cdb738df",
  userAgent: "Mozilla/5.0 (X11; Linux x86_64) Test/1.0",
  server: "apache",
  moduleStartMs: AT - 26 * 3600_000,
  optimizerStartMs: AT - 3600_000,
  optimizer: "2.1.0 (affafd3)",
  checks: [
    { name: "cache_open", pass: true },
    { name: "shared_config_version", pass: false },
  ],
  findings: [],
  counters: { page_load_count: 12, cache_hits: 3 },
  cache: { caches: [{ name: "HTTP cache", summary: "Stats(http,Compressed(Fallback(File)))" }], backend: ["Current entries: 74"], purgeEnabled: true },
  purge: { globalMs: null, urls: 2 },
  configSha256: EMPTY_SHA,
  configText: "",
  messages: [],
  ...over,
});

describe("lastWarnings", () => {
  it("keeps warnings and worse, the last ones, newest first", () => {
    const list = [msg("warning", "w1"), msg("info", "i1"), msg("error", "e1"), msg("fatal", "f1"), msg("warning", "w2")];
    expect(lastWarnings(list)).toEqual([msg("warning", "w2"), msg("fatal", "f1"), msg("error", "e1"), msg("warning", "w1")]);
    expect(lastWarnings(list, 2)).toEqual([msg("warning", "w2"), msg("fatal", "f1")]);
  });
  it("ignores junk", () => {
    expect(lastWarnings("nope")).toEqual([]);
    expect(lastWarnings([null, 3, { severity: "warning" }, { severity: 1, message: "x" }, msg("warning", "ok")])).toEqual([
      msg("warning", "ok"),
    ]);
  });
  it("keeps at most 50", () => {
    const many = Array.from({ length: 80 }, (_, i) => msg("warning", `w${i}`));
    const out = lastWarnings(many);
    expect(out).toHaveLength(50);
    expect(out[0].message).toBe("w79");
  });
});

describe("diagnosticsText", () => {
  it("is plain text in a fixed order", () => {
    const text = diagnosticsText(
      data({
        messages: [msg("info", "hello"), msg("warning", "[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] slow 1")],
        findings: [{ severity: "warning", label: "Fetch failures", message: "3 fetches failed", fix: "Check the origin." }],
      }),
      WHOLE,
    );
    expect(text).toBe(
      [
        "mod_pagespeed diagnostics",
        "Generated: 2026-10-02T10:25:51.000Z",
        "Console: whole server",
        "Build: v1.16.0-86-g0cdb738df",
        "Web server: Apache",
        "Browser: Mozilla/5.0 (X11; Linux x86_64) Test/1.0",
        "Module started: 2026-10-01T08:25:51.000Z (running for 1d 2h 0m)",
        "Optimizer: 2.1.0 (affafd3)",
        "Optimizer started: 2026-10-02T09:25:51.000Z (running for 1h 0m)",
        `Configuration SHA-256: ${EMPTY_SHA}`,
        "",
        "Findings (1, from one reading):",
        "- [warning] Fetch failures: 3 fetches failed",
        "  Fix: Check the origin.",
        "",
        "Optimizer checks:",
        "  cache_open: pass",
        "  shared_config_version: FAIL",
        "",
        "Key counters (since the module started):",
        ...KEY_COUNTERS.map((n) => `  ${n}: ${n === "page_load_count" ? "12" : n === "cache_hits" ? "3" : "not reported"}`),
        "",
        "Caches:",
        "  HTTP cache: Stats(http,Compressed(Fallback(File)))",
        "  Current entries: 74",
        "Purging: enabled; last whole-cache invalidation: none; 2 URLs in the purge set",
        "",
        "The last warning or error:",
        "[warning] [Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] slow 1",
        "",
        "Left out of this bundle:",
        "- the configuration text (its SHA-256 is above)",
        "- query strings in addresses",
        "- client addresses in log lines",
        "- cookie and authorization header values, passwords in addresses",
        "- the addresses in the purge set",
        "- informational log lines, findings that need two readings",
        "",
      ].join("\n"),
    );
  });

  it("says what it could not read", () => {
    const text = diagnosticsText(
      data({
        server: null,
        userAgent: "",
        moduleStartMs: null,
        optimizerStartMs: null,
        optimizer: "not configured",
        checks: null,
        findings: null,
        counters: null,
        cache: null,
        purge: null,
        configSha256: null,
        configText: null,
      }),
      "this host (www.example.test:80)",
      { queryStrings: false, configText: true },
    );
    expect(text).toContain("Console: this host (www.example.test:80)\n");
    expect(text).toContain("Web server: not detected\n");
    expect(text).toContain("Browser: unknown\n");
    expect(text).toContain("Module started: unknown\n");
    expect(text).toContain("Optimizer started: unknown\n");
    expect(text).toContain("Configuration SHA-256: could not be read\n");
    expect(text).toContain("Findings: could not be read\n");
    expect(text).toContain("Optimizer checks: could not be read\n");
    expect(text).toContain("Key counters: could not be read\n");
    expect(text).toContain("Caches: could not be read\n");
    expect(text).toContain("Purging: unknown; purge set could not be read\n");
    expect(text).toContain("\nWarnings and errors: none in the log\n");
    expect(text).toContain("\nConfiguration: could not be read\n");
  });

  it("says none when one reading found nothing", () => {
    const text = diagnosticsText(data({ checks: [], cache: { caches: [], backend: [], purgeEnabled: false } }), WHOLE);
    expect(text).toContain("\nFindings: none in one reading\n");
    expect(text).toContain("\nOptimizer checks: none reported\n");
    expect(text).toContain("\nCaches: none reported\nPurging: not enabled;");
  });

  it("the diagnostics bundle is plain text: one line per message, markup kept as characters", () => {
    const text = diagnosticsText(
      data({ messages: [msg("error", "line one\n[error] forged line\r<script>alert(1)</script>"), msg("warning", "second")] }),
      WHOLE,
    );
    expect(text).toContain("The last 2 warnings and errors, newest first:\n[warning] second\n");
    expect(text).toContain("[error] line one [error] forged line <script>alert(1)</script>\n");
  });

  it("removes query strings by default, everywhere, and keeps them on request", () => {
    const d = data({
      messages: [msg("error", "Fetch of https://www.example.test/a.css?v=1&session=abc failed.")],
      findings: [{ severity: "warning", label: "Slow", message: "/b.js?x=1 is slow", fix: "Look at https://example.com/?q=2#top." }],
      configText: "ModPagespeedMapOriginDomain http://origin.example.test/?k=v",
    });
    const stripped = diagnosticsText(d, WHOLE, { queryStrings: false, configText: true });
    expect(stripped).toContain("Fetch of https://www.example.test/a.css?[query removed] failed.\n");
    expect(stripped).toContain("/b.js?[query removed] is slow");
    expect(stripped).toContain("Look at https://example.com/?[query removed]#top.");
    expect(stripped).toContain("ModPagespeedMapOriginDomain http://origin.example.test/?[query removed]");
    expect(stripped).not.toMatch(/session=|v=1|x=1|q=2|k=v/);
    expect(stripped).toContain("- query strings in addresses\n");
    const kept = diagnosticsText(d, WHOLE, { queryStrings: true, configText: true });
    expect(kept).toContain("a.css?v=1&session=abc failed.");
    expect(kept).not.toContain("- query strings in addresses");
  });

  it("leaves the configuration text out unless asked, and then replaces secret-looking values", () => {
    const marker = "pagespeed-config-marker-7f3a";
    const d = data({
      configText: [
        "Options",
        `  DaemonSocketPath (dmsp)             /run/${marker}.sock`,
        "  UrlSigningKey (usk)                 s3cr3t-signing-value",
        "  DownstreamCachePurgeSecret (dcps)   hunter2",
        "  RedisPassword (rdp)                 p4ssw0rd",
        "  ApiToken (at)                       tok-123456",
        "  SomeCredential (sc)                 abc def",
      ].join("\n"),
    });
    const off = diagnosticsText(d, WHOLE);
    expect(off).not.toContain(marker);
    expect(off).not.toContain("DaemonSocketPath");
    expect(off).toContain("- the configuration text (its SHA-256 is above)\n");
    const on = diagnosticsText(d, WHOLE, { queryStrings: false, configText: true });
    expect(on).toContain("Configuration (values of options whose names suggest a secret are replaced):\nOptions\n");
    expect(on).toContain(`  DaemonSocketPath (dmsp)             /run/${marker}.sock\n`);
    expect(on).toContain("  UrlSigningKey (usk)                 [redacted]\n");
    expect(on).toContain("  DownstreamCachePurgeSecret (dcps)   [redacted]\n");
    expect(on).toContain("  RedisPassword (rdp)                 [redacted]\n");
    expect(on).toContain("  ApiToken (at)                       [redacted]\n");
    expect(on).toContain("  SomeCredential (sc)                 [redacted]\n");
    for (const secret of ["s3cr3t-signing-value", "hunter2", "p4ssw0rd", "tok-123456", "abc def"]) expect(on).not.toContain(secret);
    expect(on).toContain("- the values of options whose names suggest a secret\n");
  });

  it("never carries cookies, authorization values or passwords in addresses", () => {
    const text = diagnosticsText(
      data({
        messages: [
          msg("warning", "request headers: Cookie: sid=abc123; theme=dark"),
          msg("warning", "Authorization: Bearer abcdefghijklmnopqrstuvwxyz0123"),
          msg("error", "fetch of https://admin:pa55word@origin.example.test/x failed"),
        ],
        configText: "ModPagespeedDomain https://user:topsecret@cdn.example.test/\nSet-Cookie: a=b",
      }),
      WHOLE,
      { queryStrings: false, configText: true },
    );
    for (const secret of ["sid=abc123", "abcdefghijklmnopqrstuvwxyz0123", "pa55word", "topsecret", "a=b"]) {
      expect(text).not.toContain(secret);
    }
    expect(text).toContain("Cookie: [redacted]");
    expect(text).toContain("https://admin:[redacted]@origin.example.test/x");
  });

  it("masks public client addresses in log lines, and says so", () => {
    const exposure = (client: string) =>
      `mod_pagespeed: admin handler 'pagespeed_admin' received request from ${client}; intended for loopback.`;
    const text = diagnosticsText(
      data({
        messages: [
          msg("warning", exposure("203.0.113.7")),
          msg("warning", exposure("2001:db8::1")),
          msg("warning", exposure("[2001:db8::2]:443 and 198.51.100.9:8080")),
          msg("warning", exposure("127.0.0.1")),
          msg("warning", exposure("10.1.2.3")),
          msg("error", "[Sat, 26 Sep 2026 10:04:00 GMT] [Error] [531] build 2.8.1 hash 0123456789abcdef0123456789abcdef"),
        ],
      }),
      WHOLE,
    );
    expect(text).toContain(`[warning] ${exposure("[address removed]")}\n`);
    expect(text).toContain("received request from [[address removed]]:443 and [address removed]:8080;");
    for (const kept of ["127.0.0.1", "10.1.2.3", "10:04:00 GMT", "2.8.1", "0123456789abcdef0123456789abcdef"]) {
      expect(text).toContain(kept);
    }
    expect(text).not.toMatch(/203\.0\.113\.7|2001:db8|198\.51\.100\.9/);
    expect(text).toContain("\n- client addresses in log lines\n");
  });
});

describe("redactConfig", () => {
  it("replaces only values of options whose names suggest a secret, in the module's option list", () => {
    expect(redactConfig("  FileCachePath (afcp)  /var/cache/x\n  SslCertDirectory (assld)  /etc/ssl")).toBe(
      "  FileCachePath (afcp)  /var/cache/x\n  SslCertDirectory (assld)  /etc/ssl",
    );
    expect(redactConfig("  CacheKeyPrefix (ckp)  abc")).toBe("  CacheKeyPrefix (ckp)  [redacted]");
    expect(redactConfig("  AdminPassword (ap)  x y z\n  FetcherProxy (afp)  ***")).toBe(
      "  AdminPassword (ap)  [redacted]\n  FetcherProxy (afp)  ***",
    );
    expect(redactConfig("Version: 15: on\nFilters\nah\tAdd Head")).toBe("Version: 15: on\nFilters\nah\tAdd Head");
  });
});

describe("maskClientAddresses", () => {
  it("keeps loopback, private, link-local and unique-local addresses", () => {
    const kept = "127.0.0.1 10.1.2.3 172.18.0.4 192.168.1.1 169.254.1.1 ::1 fe80::1ff:fe23:4567:890a fd00::5";
    expect(maskClientAddresses(kept)).toBe(kept);
  });
  it("leaves times, versions, hashes and non-addresses alone", () => {
    const plain = "10:04:00 at 2026-10-02T10:25:51Z v2.8.1 1.2.3.4.5 999.1.1.1 abcd:ef sha deadbeef:cafe:1";
    expect(maskClientAddresses(plain)).toBe(plain);
  });
  it("masks public IPv4 and IPv6 addresses", () => {
    expect(maskClientAddresses("from 8.8.8.8, 2001:db8::1 and ::ffff:203.0.113.1")).toBe(
      "from [address removed], [address removed] and ::ffff:[address removed]",
    );
  });
  it("masks an IPv6 address that ends a sentence, short and full form", () => {
    expect(maskClientAddresses("received request from 2001:db8::1.")).toBe("received request from [address removed].");
    expect(maskClientAddresses("from 2001:0db8:0000:0000:0000:0000:0000:0001. Next")).toBe("from [address removed]. Next");
    expect(maskClientAddresses("from 2001:db8::1, then")).toBe("from [address removed], then");
  });
  it("keeps a mapped private IPv4 address as it is", () => {
    expect(maskClientAddresses("from ::ffff:192.168.1.1.")).toBe("from ::ffff:192.168.1.1.");
  });
});

describe("scrubSecrets and stripQueryStrings", () => {
  it("replace the whole password, also with a raw @, without a scheme or protocol-relative", () => {
    expect(scrubSecrets("http://user:p@ss@host.example/x")).toBe("http://user:[redacted]@host.example/x");
    expect(scrubSecrets("see //user:pass@cdn.example/x")).toBe("see //user:[redacted]@cdn.example/x");
    expect(scrubSecrets("redis user:pass@redis.local:6379 down")).toBe("redis user:[redacted]@redis.local:6379 down");
    expect(scrubSecrets("[Error] [531] mail a@b.example at 10:04:00")).toBe("[Error] [531] mail a@b.example at 10:04:00");
  });
  it("replace a password after a comma or semicolon, without a scheme", () => {
    expect(scrubSecrets("upstreams a.example,user:pass@redis.local:6379")).toBe(
      "upstreams a.example,user:[redacted]@redis.local:6379",
    );
    expect(scrubSecrets("cache=memcached;user:pass@mc.local:11211")).toBe("cache=memcached;user:[redacted]@mc.local:11211");
  });
  it("replace bearer tokens of any length", () => {
    expect(scrubSecrets("Bearer abc123")).toBe("Bearer [redacted]");
    expect(scrubSecrets("sent bearer x.y-z_1 to origin")).toBe("sent bearer [redacted] to origin");
    expect(scrubSecrets("Bearer eyJhbGciOiJIUzI1NiJ9.e30.sig")).toBe("Bearer [redacted]");
  });
  it("strip jsessionid path parameters with the query string", () => {
    expect(stripQueryStrings("/a.css;jsessionid=ABC123?x=1 and /b;JSESSIONID=Z9")).toBe(
      "/a.css;jsessionid=[redacted]?[query removed] and /b;jsessionid=[redacted]",
    );
  });
  it("leave ordinary text alone", () => {
    const plain = "Purging: enabled; 3 URLs in the purge set\n/var/cache/mod_pagespeed/ is fine? yes.";
    expect(scrubSecrets(plain)).toBe(plain);
    expect(stripQueryStrings(plain)).toBe(plain);
  });
  it("strip a query string up to the fragment and keep closing punctuation", () => {
    expect(stripQueryStrings("(see https://a.example/p?x=1).")).toBe("(see https://a.example/p?[query removed]).");
    expect(stripQueryStrings("/p?x=1#f")).toBe("/p?[query removed]#f");
  });
});

describe("KEY_COUNTERS", () => {
  it("names counters an Apache server reports", () => {
    const names = new Set<string>(
      JSON.parse(readFileSync(new URL("../../../test-fixtures/stat-names.apache-2.8.1.fixture.json", import.meta.url), "utf8")),
    );
    expect(KEY_COUNTERS.length).toBe(12);
    for (const n of KEY_COUNTERS) expect(names.has(n), n).toBe(true);
  });
});

describe("healthChecks and cacheOf", () => {
  it("read every check with its verdict", () => {
    expect(healthChecks({ checks: { a: { pass: true }, b: { pass: false }, c: false, d: true, e: "odd" } })).toEqual([
      { name: "a", pass: true },
      { name: "b", pass: false },
      { name: "c", pass: false },
      { name: "d", pass: true },
      { name: "e", pass: null },
    ]);
    expect(healthChecks(null)).toEqual([]);
  });
  it("read the caches, the backend lines and the purge switch", () => {
    expect(
      cacheOf({ caches: [{ name: "HTTP", summary: "Stats(\nFile)" }], backend_stats: "\nA: 1\n\nB: 2\n", purge_enabled: false }),
    ).toEqual({ caches: [{ name: "HTTP", summary: "Stats(\nFile)" }], backend: ["A: 1", "B: 2"], purgeEnabled: false });
    expect(cacheOf({})).toEqual({ caches: [], backend: [], purgeEnabled: null });
  });
});

describe("diagnosticsFileName", () => {
  it("is safe on every file system", () => {
    expect(diagnosticsFileName(AT)).toBe("mod_pagespeed-diagnostics-2026-10-02T10-25-51Z.txt");
  });
});

describe("optimizerLine", () => {
  const fulfilled = (value: DaemonHealthResponse): PromiseSettledResult<DaemonHealthResponse> => ({ status: "fulfilled", value });
  const rejected = (reason: unknown): PromiseSettledResult<DaemonHealthResponse> => ({ status: "rejected", reason });
  it("names the version and commit, or why there is none", () => {
    expect(optimizerLine(fulfilled({ status: "ok", version: "2.1.0", git_commit: "affafd3" }))).toBe("2.1.0 (affafd3)");
    expect(optimizerLine(fulfilled({ status: "ok", version: "2.1.0" }))).toBe("2.1.0");
    expect(optimizerLine(fulfilled({ status: "ok" }))).toBe("answering, version not reported");
    expect(optimizerLine(rejected(new ApiError(503, "daemon_not_configured")))).toBe("not configured");
    expect(optimizerLine(rejected(new ApiError(404, "Unknown admin page")))).toBe("not configured");
    expect(optimizerLine(rejected(new ApiError(502, "daemon_unreachable")))).toBe("configured, not answering");
    expect(optimizerLine(rejected(new ApiError(501, "endpoint_unsupported_by_daemon")))).toBe("too old to report its version");
    expect(optimizerLine(rejected(new NetworkError()))).toBe("did not answer");
    expect(optimizerLine(rejected("odd"))).toBe("did not answer");
  });
});

describe("configHash", () => {
  it("hashes the effective configuration, else the server's, else nothing", () => {
    expect(configHash({ config: "a", effective_config: "abc" })).toBe(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    );
    expect(configHash({ config: "abc", effective_config: "" })).toBe(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    );
    expect(configHash({ config: "" })).toBe(EMPTY_SHA);
    expect(configHash({})).toBeNull();
    expect(configHash(null)).toBeNull();
    expect(configHash("text")).toBeNull();
  });
});

describe("collectDiagnostics", () => {
  const ctx = { build: "v1.16.0", userAgent: "UA/1", server: "nginx" as const };
  const api = (over: Partial<DiagnosticsApi> = {}): DiagnosticsApi => ({
    getConfig: async () => ({ config: "", effective_config: "abc" }),
    getStats: async () => ({ variables: { page_load_count: 7, process_start_ms: AT - 60_000 }, maxlength: 60 }),
    daemonHealth: async () => ({ status: "ok", version: "2.1.0", git_commit: "affafd3", checks: { cache_open: { pass: true } } }),
    daemonStats: async () => ({ started_at_ms: AT - 120_000 }),
    getMessages: async () => ({ scope: "process", next: 1, messages: [{ timestamp: 0, severity: "error", message: "boom" }] }),
    getCacheStructure: async () => ({ caches: [{ name: "HTTP", summary: "File" }], purge_enabled: true }),
    getPurgeSet: async () => ({ purge_set: ["https://a.example/x"], global_invalidation_timestamp_ms: AT - 1000 }),
    ...over,
  });
  it("reads each source once and gathers the bundle's inputs", async () => {
    const d = await collectDiagnostics(api(), ctx, () => AT);
    const text = diagnosticsText(d, WHOLE);
    expect(text).toContain("Web server: nginx\n");
    expect(text).toContain("Browser: UA/1\n");
    expect(text).toContain("Module started: 2026-10-02T10:24:51.000Z (running for 1m 0s)\n");
    expect(text).toContain("Optimizer: 2.1.0 (affafd3)\n");
    expect(text).toContain("Optimizer started: 2026-10-02T10:23:51.000Z (running for 2m 0s)\n");
    expect(text).toContain("Configuration SHA-256: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
    expect(text).toContain("Optimizer checks:\n  cache_open: pass\n");
    expect(text).toContain("  page_load_count: 7\n");
    expect(text).toContain("Caches:\n  HTTP: File\n");
    expect(text).toContain("Purging: enabled; last whole-cache invalidation: 2026-10-02T10:25:50.000Z; 1 URL in the purge set\n");
    expect(text).not.toContain("a.example");
    expect(text).toContain("[error] boom\n");
    expect(d.findings).not.toBeNull();
  });
  it("reads the message log once, as the plain list, and still finds an exposed console in it", async () => {
    let grouped = 0;
    let plain = 0;
    const line =
      "mod_pagespeed: admin handler 'pagespeed_admin' received request from 203.0.113.7; intended for loopback. " +
      "Restrict access at the web-server layer.";
    const reader = {
      ...api({
        getMessages: async () => {
          plain++;
          return { scope: "process", next: 1, messages: [{ timestamp: AT - 1000, severity: "warning", message: line }] };
        },
      }),
      getMessageGroups: async () => {
        grouped++;
        return { groups: [] };
      },
    };
    const d = await collectDiagnostics(reader, ctx, () => AT);
    expect(grouped).toBe(0);
    expect(plain).toBe(1);
    expect(d.findings?.map((f) => f.label)).toContain("Admin console reachable from the network");
    expect(diagnosticsText(d, WHOLE)).not.toContain("203.0.113.7");
  });

  it("never includes the configuration text by default, only its fingerprint", async () => {
    const marker = "pagespeed-config-marker-7f3a";
    const d = await collectDiagnostics(
      api({ getConfig: async () => ({ config: `ModPagespeedDomain ${marker}-a`, effective_config: `DownstreamCachePurgeLocationPrefix ${marker}-b` }) }),
      ctx,
      () => AT,
    );
    const text = diagnosticsText(d, WHOLE);
    expect(text).toMatch(/^Configuration SHA-256: [0-9a-f]{64}$/m);
    expect(text).not.toContain(marker);
    expect(text).not.toContain("ModPagespeedDomain");
    expect(text).not.toContain("DownstreamCachePurgeLocationPrefix");
    expect(diagnosticsText(d, WHOLE, { queryStrings: false, configText: true })).toContain(`DownstreamCachePurgeLocationPrefix ${marker}-b`);
  });
  it("still produces a bundle when every read fails or answers junk", async () => {
    const fail = (status: number, text: string) => async () => {
      throw new ApiError(status, text);
    };
    const d = await collectDiagnostics(
      api({
        getConfig: fail(500, "x"),
        getStats: fail(500, "x"),
        daemonHealth: fail(502, "daemon_unreachable"),
        daemonStats: fail(502, "daemon_unreachable"),
        getMessages: async () => null as unknown as never,
        getCacheStructure: fail(403, "x"),
        getPurgeSet: fail(403, "x"),
      }),
      ctx,
      () => AT,
    );
    const text = diagnosticsText(d, WHOLE);
    expect(text).toContain("Optimizer: configured, not answering\n");
    expect(text).toContain("Configuration SHA-256: could not be read");
    expect(text).toContain("Findings: could not be read");
    expect(text).toContain("Optimizer checks: could not be read");
    expect(text).toContain("Caches: could not be read");
    expect(text).toContain("Warnings and errors: none in the log");
  });
});

describe("copyText", () => {
  it("uses the clipboard when it accepts", async () => {
    let copied = "";
    expect(await copyText("hello", { writeText: async (t) => void (copied = t) }, undefined)).toBe(true);
    expect(copied).toBe("hello");
  });
  it("falls back to a selected textarea, and removes it again", async () => {
    const calls: string[] = [];
    const area = {
      value: "",
      style: {} as Record<string, string>,
      setAttribute: () => calls.push("readonly"),
      select: () => calls.push("select"),
      remove: () => calls.push("remove"),
    };
    const doc = {
      createElement: () => area,
      body: { appendChild: () => calls.push("append") },
      execCommand: (c: string) => (calls.push(c), true),
    } as unknown as Document;
    const refusing = { writeText: async () => Promise.reject(new Error("denied")) };
    expect(await copyText("hello", refusing, doc)).toBe(true);
    expect(area.value).toBe("hello");
    expect(calls).toEqual(["readonly", "append", "select", "copy", "remove"]);
  });
  it("reports failure when nothing can copy", async () => {
    expect(await copyText("hello", undefined, undefined)).toBe(false);
    const doc = {
      createElement: () => ({ value: "", style: {}, setAttribute() {}, select() {}, remove() {} }),
      body: { appendChild() {} },
      execCommand: () => false,
    } as unknown as Document;
    expect(await copyText("hello", undefined, doc)).toBe(false);
  });
});

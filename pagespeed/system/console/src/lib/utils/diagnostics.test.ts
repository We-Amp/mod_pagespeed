// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ApiError } from "$lib/api/client";
import { NetworkError } from "$lib/api/connection";
import type { DaemonHealthResponse } from "$lib/api/types";
import {
  collectDiagnostics,
  configHash,
  copyText,
  diagnosticsText,
  lastWarnings,
  optimizerLine,
  type DiagnosticsApi,
} from "./diagnostics";

const AT = Date.UTC(2026, 9, 2, 10, 25, 51);
const EMPTY_SHA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const msg = (severity: string, message: string) => ({ severity, message });

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
    const text = diagnosticsText({
      generatedAt: AT,
      scope: "whole server",
      moduleBuild: "v1.16.0-86-g0cdb738df",
      consoleBuild: "v1.16.0-86-g0cdb738df",
      optimizer: "2.1.0 (affafd3)",
      configSha256: EMPTY_SHA,
      messages: [msg("info", "hello"), msg("warning", "[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] slow 1")],
    });
    expect(text).toBe(
      [
        "mod_pagespeed diagnostics",
        "Generated: 2026-10-02T10:25:51.000Z",
        "Console: whole server",
        "Module build: v1.16.0-86-g0cdb738df",
        "Console build: v1.16.0-86-g0cdb738df",
        "Optimizer: 2.1.0 (affafd3)",
        `Configuration SHA-256: ${EMPTY_SHA}`,
        "",
        "The last warning or error:",
        "[warning] [Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] slow 1",
        "",
      ].join("\n"),
    );
  });

  it("says when there is nothing to list or no configuration", () => {
    const text = diagnosticsText({
      generatedAt: AT,
      scope: "this host (www.example.test:80)",
      moduleBuild: "dev",
      consoleBuild: "dev",
      optimizer: "not configured",
      configSha256: null,
      messages: [],
    });
    expect(text).toContain("Configuration SHA-256: unavailable (the configuration could not be read)\n");
    expect(text).toContain("\nWarnings and errors: none in the log\n");
  });

  it("the diagnostics bundle is plain text: one line per message, markup kept as characters", () => {
    const text = diagnosticsText({
      generatedAt: AT,
      scope: "whole server",
      moduleBuild: "b",
      consoleBuild: "b",
      optimizer: "x",
      configSha256: EMPTY_SHA,
      messages: [msg("error", "line one\n[error] forged line\r<script>alert(1)</script>"), msg("warning", "second")],
    });
    expect(text).toContain("The last 2 warnings and errors, newest first:\n[warning] second\n");
    expect(text).toContain("[error] line one [error] forged line <script>alert(1)</script>\n");
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
  const api = (over: Partial<DiagnosticsApi> = {}): DiagnosticsApi => ({
    getConfig: async () => ({ config: "", effective_config: "abc" }),
    daemonHealth: async () => ({ status: "ok", version: "2.1.0", git_commit: "affafd3" }),
    getMessages: async () => ({ scope: "process", next: 1, messages: [{ timestamp: 0, severity: "error", message: "boom" }] }),
    ...over,
  });
  it("reads the three sources once and assembles the bundle", async () => {
    const text = await collectDiagnostics(api(), "whole server", "v1.16.0", () => AT);
    expect(text).toContain("Optimizer: 2.1.0 (affafd3)\n");
    expect(text).toContain("Configuration SHA-256: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
    expect(text).toContain("[error] boom\n");
  });
  it("never includes the configuration text, only its fingerprint", async () => {
    const marker = "pagespeed-config-marker-7f3a";
    const text = await collectDiagnostics(
      api({ getConfig: async () => ({ config: `ModPagespeedDomain ${marker}-a`, effective_config: `DownstreamCachePurgeLocationPrefix ${marker}-b` }) }),
      "whole server",
      "v1.16.0",
      () => AT,
    );
    expect(text).toMatch(/^Configuration SHA-256: [0-9a-f]{64}$/m);
    expect(text).not.toContain(marker);
    expect(text).not.toContain("ModPagespeedDomain");
    expect(text).not.toContain("DownstreamCachePurgeLocationPrefix");
  });
  it("still produces a bundle when every read fails or answers junk", async () => {
    const text = await collectDiagnostics(
      api({
        getConfig: async () => {
          throw new ApiError(500, "x");
        },
        daemonHealth: async () => {
          throw new ApiError(502, "daemon_unreachable");
        },
        getMessages: async () => null as unknown as never,
      }),
      "whole server",
      "v1.16.0",
      () => AT,
    );
    expect(text).toContain("Optimizer: configured, not answering\n");
    expect(text).toContain("Configuration SHA-256: unavailable");
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

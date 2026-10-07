// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import { displayHost, knownPerVhostScope, resolveScope, scopeLine, statisticsScopeLine } from "./config-scope";

describe("scopeLine", () => {
  it("names the vhost for a per-vhost console", () => {
    expect(scopeLine({ scope: "vhost", host: "site-b.test:81" })).toBe(
      "Configuration of this virtual host (site-b.test:81)");
  });
  it("names the aggregate for the global console", () => {
    expect(scopeLine({ scope: "global", host: "" })).toBe(
      "Server-wide configuration (all virtual hosts)");
  });
  it("falls back when the backend predates the scope field", () => {
    expect(scopeLine({})).toBe("Configuration");
  });
});

describe("statisticsScopeLine", () => {
  it("names the aggregate for the global console", () => {
    expect(statisticsScopeLine(true)).toBe(
      "Aggregate statistics across all virtual hosts (process-wide)");
  });
  it("names the vhost for a per-vhost console", () => {
    expect(statisticsScopeLine(false)).toBe(
      "Statistics of this virtual host — separate from other hosts only " +
      "when per-virtual-host statistics are enabled");
  });
});

describe("displayHost", () => {
  it("keeps a named host and its port", () => {
    expect(displayHost("www.example.test:80", "console.test")).toBe("www.example.test:80");
    expect(displayHost("[::1]:8080", "console.test")).toBe("[::1]:8080");
  });
  it("drops a zero port", () => {
    expect(displayHost("www.example.test:0", "console.test")).toBe("www.example.test");
  });
  it("falls back to the host the browser used when the name is empty", () => {
    expect(displayHost(":0", "console.test:8080")).toBe("console.test:8080");
    expect(displayHost("", "console.test")).toBe("console.test");
    expect(displayHost(undefined, "console.test")).toBe("console.test");
  });
  it("treats a non-string host (the daemon is untrusted) as absent", () => {
    expect(displayHost(42 as unknown as string, "console.test")).toBe("console.test");
    expect(displayHost({ toString: () => "evil" } as unknown as string, "console.test")).toBe("console.test");
  });
});

describe("resolveScope", () => {
  it("the configuration's own scope wins over the admin path", () => {
    expect(resolveScope(false, { scope: "global", host: "" }, "console.test")).toEqual({ isGlobal: true, host: "" });
    expect(resolveScope(true, { scope: "vhost", host: ":0" }, "console.test")).toEqual({
      isGlobal: false,
      host: "console.test",
    });
  });
  it("without a configuration answer the admin path decides", () => {
    expect(resolveScope(true, null, "console.test")).toEqual({ isGlobal: true, host: "" });
    expect(resolveScope(false, null, "console.test")).toEqual({ isGlobal: false, host: "console.test" });
  });
});

describe("knownPerVhostScope", () => {
  it("is not known before /config answers, even when the path guesses per-vhost", () => {
    expect(knownPerVhostScope(null, false)).toBe(false);
  });
  it("is known once /config has settled on a per-vhost scope", () => {
    expect(knownPerVhostScope({ scope: "vhost", host: "www.example.test:80" }, false)).toBe(true);
  });
  it("is not known on the global console, even though /config has answered", () => {
    expect(knownPerVhostScope({ scope: "global", host: "" }, true)).toBe(false);
  });
});

describe("statisticsScopeLine with a host", () => {
  it("names the host of a per-vhost console", () => {
    expect(statisticsScopeLine(false, "www.example.test")).toBe(
      "Statistics of this virtual host (www.example.test) — separate from other hosts only " +
      "when per-virtual-host statistics are enabled");
  });
  it("ignores the host on the global console", () => {
    expect(statisticsScopeLine(true, "www.example.test")).toBe(
      "Aggregate statistics across all virtual hosts (process-wide)");
  });
});

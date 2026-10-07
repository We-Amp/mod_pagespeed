// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { consoleHostName, hostRow, hostSavingsView, siteRow } from "./host-savings";

const row = (host: unknown, hits: unknown, original: unknown, optimized: unknown) => ({
  host,
  hits,
  original_bytes: original,
  optimized_bytes: optimized,
});

describe("hostSavingsView", () => {
  it("reads the optimizer's rows, most served first, with the saving and its share", () => {
    const view = hostSavingsView({
      hosts: [row("static.example.com", 2, 500000, 100000), row("www.example.com", 3, 200000, 200000)],
      limit: 32,
      other: { hits: 1, original_bytes: 300, optimized_bytes: 200 },
    });
    expect(view?.hosts.map((r) => r.host)).toEqual(["www.example.com", "static.example.com"]);
    expect(view?.hosts[1]).toEqual({ host: "static.example.com", hits: 2, original: 500000, served: 100000, saved: 400000, percent: 80 });
    expect(view?.hosts[0].saved).toBe(0);
    expect(view?.other).toMatchObject({ hits: 1, saved: 100 });
  });

  it("hosts and other still add up: a bad host name folds into other, a repeated host is summed, malformed counters drop the row", () => {
    const view = hostSavingsView({
      hosts: [
        row("<img src=x onerror=alert(1)>", 9, 900, 100),
        row("WWW.Example.COM", 2, 100, 50),
        row("www.example.com", 5, 100, 50),
        row("neg.example", -1, 100, 50),
        row("str.example", "3", 100, 50),
        row(7, 1, 1, 1),
        "junk",
        null,
      ],
      other: { hits: 4, original_bytes: 4000, optimized_bytes: 1000 },
    });
    expect(view?.hosts.map((r) => r.host)).toEqual(["www.example.com"]);
    expect(view?.hosts[0]).toEqual({ host: "www.example.com", hits: 7, original: 200, served: 100, saved: 100, percent: 50 });
    // The markup row and the numeric host, both with well-formed counters.
    expect(view?.other).toEqual({ host: "", hits: 14, original: 4901, served: 1101, saved: 3800, percent: 78 });
  });

  it("a folded row makes an other row where the optimizer sent none", () => {
    const view = hostSavingsView({ hosts: [row("bad host", 2, 100, 40), row("a.test", 1, 10, 5)] });
    expect(view?.hosts.map((r) => r.host)).toEqual(["a.test"]);
    expect(view?.other).toMatchObject({ hits: 2, original: 100, served: 40, saved: 60 });
  });

  it("a saving never goes below zero or above the original", () => {
    const view = hostSavingsView({ hosts: [row("a.test", 1, 100, 150)] });
    expect(view?.hosts[0]).toMatchObject({ saved: 0, percent: 0 });
  });

  it("an other row with nothing in it is no row", () => {
    expect(hostSavingsView({ hosts: [], other: { hits: 0, original_bytes: 0, optimized_bytes: 0 } })?.other).toBeNull();
  });

  it("no block, or a block without a hosts list, is no data", () => {
    for (const bad of [undefined, null, 3, "x", [], {}, { hosts: "x" }]) expect(hostSavingsView(bad)).toBeNull();
  });
});

describe("hostRow", () => {
  const view = hostSavingsView({ hosts: [row("www.example.com", 3, 200, 100)] });
  it("finds a host exactly, never by suffix", () => {
    expect(hostRow(view, "www.example.com")?.hits).toBe(3);
    expect(hostRow(view, "example.com")).toBeNull();
    expect(hostRow(view, null)).toBeNull();
    expect(hostRow(null, "www.example.com")).toBeNull();
  });
});

describe("consoleHostName", () => {
  it("a per-vhost console's host as the optimizer names it: lowercase, no port", () => {
    expect(consoleHostName("Static.Example.com:80")).toBe("static.example.com");
    expect(consoleHostName("www.example.test")).toBe("www.example.test");
    expect(consoleHostName("Www.Example.Test.:8080")).toBe("www.example.test");
    expect(consoleHostName("[2001:DB8::1]:443")).toBe("[2001:db8::1]");
  });
  it("an unnamed server context has none", () => {
    expect(consoleHostName(":0")).toBeNull();
    expect(consoleHostName("")).toBeNull();
  });
});

describe("the module's site marker", () => {
  it("is undefined without the key, the normalised host or \"\" when usable, null when not", () => {
    expect(hostSavingsView({ hosts: [] })?.site).toBeUndefined();
    expect(hostSavingsView({ hosts: [], site: "Example.TEST" })?.site).toBe("example.test");
    expect(hostSavingsView({ hosts: [], site: "" })?.site).toBe("");
    for (const bad of [null, 7, {}, "bad host", "-", "<img src=x>"]) {
      expect(hostSavingsView({ hosts: [], site: bad })?.site, String(bad)).toBeNull();
    }
  });
});

describe("siteRow", () => {
  const unmarked = hostSavingsView({ hosts: [row("www.example.com", 3, 200, 100), row("static.example.com", 2, 500, 100)] });
  const marked = (site: unknown, hosts = [row("example.com", 4, 400, 100)]) =>
    hostSavingsView({ hosts, limit: 32, other: { hits: 9, original_bytes: 900, optimized_bytes: 300 }, site });

  it("without a marker (an older module) matches the console's own name only", () => {
    expect(siteRow(unmarked, "static.example.com")).toEqual({ row: unmarked?.hosts[1], host: "static.example.com" });
    expect(siteRow(unmarked, "other.example.com")).toEqual({ row: null, host: "other.example.com" });
    expect(siteRow(hostSavingsView({ hosts: [row("example.com", 4, 400, 100)] }), "www.example.com").row).toBeNull();
  });

  it("with a marker shows the row it names, whatever name the console was opened under", () => {
    const view = marked("example.com");
    expect(siteRow(view, "www.example.com")).toEqual({ row: view?.hosts[0], host: "example.com" });
  });

  it("with a marker but no such row stays muted and names the marked site", () => {
    expect(siteRow(marked("example.com", []), "www.example.com")).toEqual({ row: null, host: "example.com" });
    expect(siteRow(marked("other.example.com"), "example.com")).toEqual({ row: null, host: "other.example.com" });
  });

  it("with an empty or unusable marker stays muted, names nobody and never matches by name", () => {
    for (const site of ["", null, 7, "bad host"]) {
      expect(siteRow(marked(site), "example.com"), String(site)).toEqual({ row: null, host: null });
    }
  });

  it("without per-host data names the console's own host and no row", () => {
    expect(siteRow(null, "www.example.com")).toEqual({ row: null, host: "www.example.com" });
  });
});

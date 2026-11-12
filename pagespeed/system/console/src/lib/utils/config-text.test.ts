// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import {
  configView,
  differingLines,
  filterEnabled,
  lineKeys,
  lineMatcher,
  optionValue,
  parseConfigText,
  searchableLines,
} from "./config-text";

const dir = fileURLToPath(new URL("../../../test-fixtures/rig/", import.meta.url));
const load = (name: string): { config: string; effective_config: string } =>
  JSON.parse(readFileSync(dir + name + ".json", "utf8"));

const rigGlobal = load("config.global");
const rigVhost = load("config.vhost-a");

describe("parseConfigText on the captured server text", () => {
  for (const [name, capture] of [
    ["whole server", rigGlobal],
    ["one host", rigVhost],
  ] as const) {
    for (const leaf of ["config", "effective_config"] as const) {
      it(`${name}, ${leaf}: filters, options and the rest`, () => {
        const parsed = parseConfigText(capture[leaf]);
        expect(parsed.structured).toBe(true);
        expect(parsed.version).toBe("15");
        expect(parsed.state).toBe("on");
        // Counted from the text itself: one filter per "id<TAB>name" line, one option per indented line.
        const text = capture[leaf];
        expect(parsed.filters).toHaveLength(text.split("\n").filter((l) => /^\S+\t/.test(l)).length);
        expect(parsed.filters).toHaveLength(30);
        expect(parsed.filters[0]).toMatchObject({ id: "ah", name: "Add Head" });
        // The server's name ends in a space; the list does not.
        expect(parsed.filters.find((f) => f.id === "fc")?.name).toBe("Fallback Rewrite Css");
        expect(parsed.optionList).toHaveLength(8);
        expect(parsed.options).toEqual({
          DaemonSocketPath: "/run/pagespeed-optimizer/notify.sock",
          DaemonVolumePath: "/var/cache/pagespeed-optimizer/v3/cache",
          EnableRewriting: "1",
          FileCachePath: "/var/cache/mod_pagespeed/",
          InPlaceResourceOptimization: "True",
          LogDir: "/var/log/pagespeed",
          SslCertDirectory: "/etc/ssl/certs",
          StatisticsLogging: "True",
        });
        expect(parsed.blocks.map((b) => (b.kind === "text" ? (b.title ?? b.lines[0].text) : b.title))).toEqual([
          "Version: 15: on",
          "Filters",
          "Options",
          "Domain Lawyer",
          "Invalidation Timestamp: (none)",
        ]);
        // Every line is kept, in order.
        expect(parsed.lines.join("\n")).toBe(text);
      });
    }
  }

  it("reads an option by name or by id, and says when one was not set", () => {
    const parsed = parseConfigText(rigGlobal.config);
    expect(optionValue(parsed, "FileCachePath")).toBe("/var/cache/mod_pagespeed/");
    expect(optionValue(parsed, "ipro")).toBe("True");
    expect(optionValue(parsed, "RewriteLevel")).toBeUndefined();
    expect(optionValue(parsed, "toString")).toBeUndefined();
  });

  it("knows a filter by id or by name", () => {
    const parsed = parseConfigText(rigGlobal.config);
    expect(filterEnabled(parsed, "rj")).toBe(true);
    expect(filterEnabled(parsed, "recompress jpeg")).toBe(true);
    expect(filterEnabled(parsed, "lazyload_images")).toBe(false);
  });

  it("the two captured texts are the same, so no line differs", () => {
    const a = parseConfigText(rigGlobal.config);
    const b = parseConfigText(rigGlobal.effective_config);
    expect(differingLines(a, b).size).toBe(0);
    expect(differingLines(b, a).size).toBe(0);
  });

  it("searches every content line, never a heading", () => {
    const parsed = parseConfigText(rigGlobal.config);
    const lines = searchableLines(parsed);
    // Version, 30 filters, 8 options, the timestamp; not "Filters", "Options", "Domain Lawyer".
    expect(lines).toHaveLength(40);
    expect(lines.map((i) => parsed.lines[i])).not.toContain("Filters");
    const match = lineMatcher("cache path");
    expect(lines.filter((i) => match(parsed.lines[i])).map((i) => parsed.lines[i].trim())).toEqual([
      "DaemonVolumePath (dmvp)             /var/cache/pagespeed-optimizer/v3/cache",
      "FileCachePath (afcp)                /var/cache/mod_pagespeed/",
    ]);
  });
});

describe("parseConfigText on other shapes", () => {
  const TEXT = [
    "Version: 15: off",
    "",
    "Filters",
    "rj\tRecompress Jpeg",
    "",
    "Options",
    "  RewriteLevel (l)  CoreFilters",
    "  Secret (sk)       ***",
    "  Empty (em)",
    "",
    "Domain Lawyer",
    "  http://cdn.example.com/ Auth",
    "Experiment id=1;percent=50",
    "",
    "Invalidation Timestamp: Tue, 10 Nov 2026 10:00:00 GMT (1794304800000)",
    "",
  ].join("\n");

  it("an empty value, a redacted value and text after the Domain Lawyer heading", () => {
    const parsed = parseConfigText(TEXT);
    expect(parsed.structured).toBe(true);
    expect(parsed.state).toBe("off");
    expect(parsed.options).toEqual({ RewriteLevel: "CoreFilters", Secret: "***", Empty: "" });
    const lawyer = parsed.blocks.find((b) => b.kind === "text" && b.title === "Domain Lawyer");
    expect(lawyer?.kind === "text" && lawyer.lines.map((l) => l.text.trim())).toEqual([
      "http://cdn.example.com/ Auth",
      "Experiment id=1;percent=50",
    ]);
  });

  it("no filters and no options set: still the server's shape", () => {
    const parsed = parseConfigText("Version: 15: on\n\nFilters\n\nOptions\n\nDomain Lawyer\n\nInvalidation Timestamp: (none)");
    expect(parsed.structured).toBe(true);
    expect(parsed.filters).toEqual([]);
    expect(parsed.options).toEqual({});
    expect(searchableLines(parsed).map((i) => parsed.lines[i])).toEqual([
      "Version: 15: on",
      "Invalidation Timestamp: (none)",
    ]);
  });

  it("directives, not the server's dump: shown as text", () => {
    const parsed = parseConfigText("ModPagespeed on\nModPagespeedRewriteLevel CoreFilters");
    expect(parsed.structured).toBe(false);
    expect(parsed.version).toBeNull();
    expect(parsed.filters).toEqual([]);
    expect(parsed.options).toEqual({});
    expect(parsed.blocks).toHaveLength(1);
    expect(searchableLines(parsed)).toEqual([0, 1]);
  });

  it("a malformed line inside Filters or Options: shown as text, the good lines still read", () => {
    const badFilter = parseConfigText("Filters\nrj\tRecompress Jpeg\nnot a filter line\n\nOptions\n  A (a)  1");
    expect(badFilter.structured).toBe(false);
    expect(badFilter.filters.map((f) => f.id)).toEqual(["rj"]);
    const badOption = parseConfigText("Filters\n\nOptions\n  A (a)  1\nB without an id");
    expect(badOption.structured).toBe(false);
    expect(badOption.options).toEqual({ A: "1" });
  });

  it("only one of the two sections: text", () => {
    expect(parseConfigText("Filters\nrj\tRecompress Jpeg").structured).toBe(false);
    expect(parseConfigText("Options\n  A (a)  1").structured).toBe(false);
  });

  it("empty text and Windows line ends", () => {
    const empty = parseConfigText("");
    expect(empty.blocks).toEqual([]);
    expect(searchableLines(empty)).toEqual([]);
    const crlf = parseConfigText("Filters\r\nrj\tRecompress Jpeg\r\n\r\nOptions\r\n  A (a)  1\r\n");
    expect(crlf.structured).toBe(true);
    expect(crlf.options).toEqual({ A: "1" });
  });
});

describe("differingLines", () => {
  it("a heading only one text has is not counted: only lines a search can match", () => {
    const server = parseConfigText("Filters\n\nOptions\n  A (a)  1");
    const effective = parseConfigText("Filters\n\nOptions\n  A (a)  1\n\nURL cache invalidation entries\n  /x 1");
    expect([...differingLines(effective, server)].map((i) => effective.lines[i].trim())).toEqual(["/x 1"]);
  });

  it("ignores how the server lined the values up", () => {
    const server = parseConfigText("Filters\n\nOptions\n  A (a)  1\n  LongerName (ln)  2");
    const effective = parseConfigText("Filters\n\nOptions\n  A (a)  1");
    expect(lineKeys(effective)[3]).toBe("option A=1");
    expect([...differingLines(effective, server)]).toEqual([]);
    expect([...differingLines(server, effective)]).toEqual([4]);
  });

  it("a changed value and an extra filter differ", () => {
    const server = parseConfigText("Filters\nrj\tRecompress Jpeg\n\nOptions\n  A (a)  1");
    const effective = parseConfigText("Filters\nrj\tRecompress Jpeg\nri\tResize Images\n\nOptions\n  A (a)  2");
    expect([...differingLines(effective, server)].map((i) => effective.lines[i].trim())).toEqual([
      "ri\tResize Images",
      "A (a)  2",
    ]);
  });
});

describe("lineMatcher", () => {
  it("every word, any case, any spacing", () => {
    expect(lineMatcher("")("anything")).toBe(true);
    expect(lineMatcher("  ")("anything")).toBe(true);
    expect(lineMatcher("JPEG recompress")("rj\tRecompress Jpeg")).toBe(true);
    expect(lineMatcher("jpeg png")("rj\tRecompress Jpeg")).toBe(false);
    expect(lineMatcher("(afcp) /var")("  FileCachePath (afcp)     /var/cache/")).toBe(true);
  });
});

describe("configView", () => {
  const server = parseConfigText(rigGlobal.config);

  it("nothing to narrow: every block, the empty Domain Lawyer included", () => {
    const view = configView(server, "  ");
    expect(view.blocks).toBe(server.blocks);
    expect(view).toMatchObject({ shown: 40, total: 40 });
  });

  it("a query keeps the matching lines and drops the blocks it empties", () => {
    const view = configView(server, "jpeg");
    expect(view.blocks.map((b) => b.kind)).toEqual(["filters"]);
    const filters = view.blocks[0].kind === "filters" ? view.blocks[0].filters : [];
    expect(filters.map((f) => f.id)).toEqual(["jp", "jw", "pj", "js", "rj"]);
    expect(view).toMatchObject({ shown: 5, total: 40 });
    expect(configView(server, "no such thing")).toMatchObject({ blocks: [], shown: 0, total: 40 });
  });

  it("text of another shape is filtered line by line: a line that did not parse still matches", () => {
    const cases: Array<[string, string, string]> = [
      ["a malformed filter line", "Filters\nrj\tRecompress Jpeg\ncc Combine Css no tab\n\nOptions\n  A (a)  1", "no tab"],
      ["a malformed option line", "Filters\n\nOptions\n  A (a)  1\nB without an id", "without"],
      ["a section glued to the one before", "Filters\nrj\tRecompress Jpeg\nOptions\n  Foo (f)  1", "foo"],
    ];
    for (const [name, text, query] of cases) {
      const parsed = parseConfigText(text);
      expect(parsed.structured, name).toBe(false);
      const view = configView(parsed, query);
      expect(view.shown, name).toBe(1);
      expect(view.total, name).toBe(text.split("\n").filter((l) => l.trim() !== "").length);
      expect(view.blocks, name).toHaveLength(1);
      expect(view.blocks[0].kind === "text" && view.blocks[0].lines.map((l) => l.text.trim().toLowerCase()), name).toEqual([
        expect.stringContaining(query),
      ]);
    }
  });
});

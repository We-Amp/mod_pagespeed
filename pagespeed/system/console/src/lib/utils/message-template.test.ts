// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { messageLineBody, messageLineTimeMs, messageTemplate } from "./message-template";

// A byte-identical copy of the module's fixture, kept inside the console tree
// so the tests run from a checkout of the console directory alone. The
// module's admin_site_test reads the original, so both implementations of the
// rules stay identical; the drift test below compares the two copies whenever
// the module's tree is present.
const FIXTURE = fileURLToPath(
  new URL("../../../test-fixtures/message_templates.tsv", import.meta.url),
);
const MODULE_FIXTURE = fileURLToPath(
  new URL("../../../../../../test/pagespeed/system/testdata/message_templates.tsv", import.meta.url),
);
const MODULE_FIXTURE_PRESENT = existsSync(MODULE_FIXTURE);

function fixtureRows(): Array<[string, string]> {
  const rows: Array<[string, string]> = [];
  for (let row of readFileSync(FIXTURE, "utf8").split("\n")) {
    if (row.endsWith("\r")) row = row.slice(0, -1);
    if (row === "" || row.startsWith("#")) continue;
    const tab = row.indexOf("\t");
    expect(tab, row).toBeGreaterThan(-1);
    rows.push([row.slice(0, tab), row.slice(tab + 1)]);
  }
  return rows;
}

describe("message templates", () => {
  it("matches every pair in the module's fixture", () => {
    const rows = fixtureRows();
    expect(rows).toHaveLength(17);
    for (const [line, expected] of rows) {
      expect(messageTemplate(messageLineBody(line)), line).toBe(expected);
    }
  });

  // Skipped when only the console directory is checked out: the module's
  // fixture is not there to compare against.
  it.skipIf(!MODULE_FIXTURE_PRESENT)(
    "keeps its fixture copy byte-identical to the module's (skipped without the module tree)",
    () => {
      expect(readFileSync(FIXTURE).equals(readFileSync(MODULE_FIXTURE))).toBe(true);
    },
  );

  it("folds numbers, URLs and ids exactly as the module does", () => {
    const cases: Array<[string, string]> = [
      ["took 250 ms", "took N ms"],
      ["v1.16.0", "vN.N.N"],
      ["250ms", "Nms"],
      ["see https://a.test/x?y=1", "see URL"],
      ["see HTTP://A.TEST/", "see URL"],
      ["'http://a.test/b'", "'URL'"],
      ["<http://a.test/b>", "<URL>"],
      ["id ecb2343a75c6a0b7", "id ID"],
      ["id 0x1F", "id ID"],
      ["id 12345678", "id N"],
      ["deadbeef", "deadbeef"],
      ["abc1def", "abcNdef"],
      ["0x", "Nx"],
      ["0X1G", "NXNG"],
      ["xhttp://a", "xhttp://a"],
      ["ftp://host/7", "ftp://host/N"],
    ];
    for (const [input, expected] of cases) expect(messageTemplate(input), input).toBe(expected);
  });

  it("trims trailing whitespace and copies non-ASCII characters", () => {
    expect(messageTemplate("done  \t\r")).toBe("done");
    expect(messageTemplate("")).toBe("");
    expect(messageTemplate("  lead")).toBe("  lead");
    expect(messageTemplate("Größe 42")).toBe("Größe N");
  });

  it("stays linear on adversarial input: 100 kB with long inner runs finishes well under a second", () => {
    const spaces = " ".repeat(100_000);
    const start = performance.now();
    // A long run of spaces that is NOT at the end, then one that is.
    expect(messageTemplate(`a${spaces}b`)).toBe(`a${spaces}b`);
    expect(messageTemplate(`x${spaces}y${spaces}`)).toBe(`x${spaces}y`);
    // A long URL-like run and a long digit run.
    expect(messageTemplate(`see https://a.test/${".,)".repeat(33_000)} ok`)).toBe("see URL ok");
    expect(messageTemplate(`n ${"9".repeat(100_000)}`)).toBe("n N");
    expect(performance.now() - start).toBeLessThan(500);
  });

  it("strips the time, level, pid and file:line header, and nothing else", () => {
    expect(messageLineBody("[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] hello")).toBe("hello");
    expect(messageLineBody("[Wed Jan 01 00:00:00 2014] [Info] [00000] hello")).toBe("hello");
    expect(messageLineBody("[Fri, 02 Oct 2026 10:25:51 GMT] [Error] [7] [a/b.cc:12] parse")).toBe("parse");
    expect(messageLineBody("[Fri, 02 Oct 2026 10:25:51 GMT] [Info] x")).toBe("x");
    expect(messageLineBody("[t] [Info] [abc] [a.cc:1] x")).toBe("[abc] [a.cc:1] x");
    expect(messageLineBody("  at frame 2")).toBe("  at frame 2");
    expect(messageLineBody("[t] [Notice] [1] x")).toBe("[t] [Notice] [1] x");
    expect(messageLineBody("[t][Info] x")).toBe("[t][Info] x");
  });

  it("reads the header's time; a line without a header or with an unreadable time has none", () => {
    expect(messageLineTimeMs("[Fri, 02 Oct 2026 10:25:51 GMT] [Warning] [531] hello")).toBe(1790936751000);
    expect(messageLineTimeMs("  at frame 2")).toBeNull();
    expect(messageLineTimeMs("[not a time] [Info] [1] x")).toBeNull();
    expect(messageLineTimeMs("")).toBeNull();
  });
});

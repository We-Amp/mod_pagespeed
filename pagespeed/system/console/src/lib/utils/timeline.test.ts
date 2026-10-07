// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { LogRow } from "./logs-buffer";
import { EMPTY_MESSAGE_TEXT } from "./message-groups";
import {
  filterEntries,
  groupTimeline,
  isKnownTimelineLevel,
  levelCounts,
  moduleEntries,
  optimizerEntries,
  parseSourceFilter,
  streamNotes,
  timelineLevelFilter,
  type TimelineEntry,
} from "./timeline";

const WORD: Record<string, string> = { fatal: "Fatal", error: "Error", warning: "Warning", info: "Info" };
const msg = (severity: string, time: string, text: string) => ({
  severity,
  message: `[${time}] [${WORD[severity]}] [531] ${text}`,
});
const row = (id: number, ts: number, level: string, message: string, source = "worker", module = "worker"): LogRow => ({
  kind: "entry",
  id,
  seq: id,
  ts,
  level,
  source,
  module,
  message,
  haystack: message.toLowerCase(),
});
const entry = (id: string, source: "module" | "optimizer", level: string, ts: number | null, text: string, where = ""): TimelineEntry => ({
  id,
  source,
  level,
  ts,
  text,
  where,
});

describe("levels", () => {
  it("absent: warnings and worse", () => {
    expect(timelineLevelFilter(null)).toEqual({ fatal: true, error: true, warning: true, info: false, debug: false });
  });
  it("a known level: that level and worse", () => {
    expect(timelineLevelFilter("error")).toEqual({ fatal: true, error: true, warning: false, info: false, debug: false });
    expect(timelineLevelFilter("debug")).toEqual({ fatal: true, error: true, warning: true, info: true, debug: true });
  });
  it("an unknown level: everything", () => {
    expect(timelineLevelFilter("bogus")).toEqual({ fatal: true, error: true, warning: true, info: true, debug: true });
    expect(isKnownTimelineLevel("__proto__")).toBe(false);
  });
});

describe("parseSourceFilter", () => {
  it("module or optimizer; anything else is both", () => {
    expect(parseSourceFilter("module")).toBe("module");
    expect(parseSourceFilter("optimizer")).toBe("optimizer");
    for (const v of [null, "both", "Module", "x"]) expect(parseSourceFilter(v)).toBe("both");
  });
});

describe("moduleEntries", () => {
  it("strips the line header and reads the time and the level", () => {
    const [e] = moduleEntries([msg("warning", "Fri, 02 Oct 2026 10:25:51 GMT", "Slow origin")]);
    expect(e).toMatchObject({ source: "module", level: "warning", text: "Slow origin", where: "", ts: Date.UTC(2026, 9, 2, 10, 25, 51) });
  });

  it("an entry without a text shows the shared empty-message text, and an unknown severity is info", () => {
    const [e] = moduleEntries([{ severity: "bogus", message: 7 as unknown as string }]);
    expect(e).toMatchObject({ level: "info", text: EMPTY_MESSAGE_TEXT, ts: null });
  });

  it("an entry keeps its id as newer messages arrive in front of it", () => {
    const older = msg("info", "Fri, 02 Oct 2026 10:00:00 GMT", "a");
    const newer = msg("info", "Fri, 02 Oct 2026 10:00:01 GMT", "b");
    expect(moduleEntries([newer, older])[1].id).toBe(moduleEntries([older])[0].id);
  });
});

describe("optimizerEntries", () => {
  it("keeps entries, leaves markers out, and makes control characters visible", () => {
    const rows: LogRow[] = [row(0, 1000, "info", "a\nb"), { kind: "restart", id: 1 }, row(2, 0, "trace", "x", "chrome", "")];
    const out = optimizerEntries(rows);
    expect(out).toHaveLength(2);
    expect(out[0]).toMatchObject({ id: "o0", source: "optimizer", level: "info", ts: 1000, text: "a⏎b", where: "worker/worker" });
    expect(out[1]).toMatchObject({ id: "o2", level: "trace", ts: null, where: "chrome" });
  });
});

describe("streamNotes", () => {
  it("counts restarts, dropped and unrecorded entries", () => {
    const rows: LogRow[] = [
      { kind: "restart", id: 0 },
      { kind: "gap", id: 1, fromSeq: 1, toSeq: 12 },
      { kind: "shed", id: 2, count: 4 },
      row(3, 1, "info", "x"),
    ];
    expect(streamNotes(rows)).toEqual({ restarts: 1, dropped: 10, notRecorded: 4 });
  });
});

describe("filterEntries", () => {
  const all = [
    entry("m0", "module", "warning", 3, "Slow origin https://a.test/x"),
    entry("o0", "optimizer", "debug", 2, "parsed", "worker/config"),
    entry("o1", "optimizer", "trace", 1, "a new level", "worker/x"),
  ];
  const every = timelineLevelFilter("debug");

  it("by source", () => {
    expect(filterEntries(all, { source: "module", levels: every, text: "" }).map((e) => e.id)).toEqual(["m0"]);
    expect(filterEntries(all, { source: "optimizer", levels: every, text: "" }).map((e) => e.id)).toEqual(["o0", "o1"]);
    expect(filterEntries(all, { source: "both", levels: every, text: "" })).toHaveLength(3);
  });

  it("by level, always showing a level this console predates", () => {
    expect(filterEntries(all, { source: "both", levels: timelineLevelFilter("warning"), text: "" }).map((e) => e.id)).toEqual(["m0", "o1"]);
  });

  it("by text, ignoring case, over the text and the optimizer's source/module", () => {
    expect(filterEntries(all, { source: "both", levels: every, text: "  CONFIG " }).map((e) => e.id)).toEqual(["o0"]);
    expect(filterEntries(all, { source: "both", levels: every, text: "slow" }).map((e) => e.id)).toEqual(["m0"]);
  });
});

describe("levelCounts", () => {
  it("counts per level; a level this console predates counts as debug", () => {
    const counts = levelCounts([entry("a", "module", "error", 1, "x"), entry("b", "optimizer", "trace", 1, "y"), entry("c", "optimizer", "debug", 1, "z")]);
    expect(counts).toEqual({ fatal: 0, error: 1, warning: 0, info: 0, debug: 2 });
  });
});

describe("groupTimeline", () => {
  it("folds repeats of one template within a source, never across, newest first", () => {
    const rows = groupTimeline(
      [
        entry("m1", "module", "warning", 10, "No permission to rewrite 'https://a.test/x.js'"),
        entry("m0", "module", "warning", 5, "No permission to rewrite 'https://a.test/x.js?v=2'"),
        entry("o0", "optimizer", "warning", 7, "No permission to rewrite 'https://a.test/x.js'"),
      ],
      true,
    );
    expect(rows.map((r) => [r.source, r.count])).toEqual([["module", 2], ["optimizer", 1]]);
    expect(rows[0].text).toBe("No permission to rewrite 'https://a.test/x.js'");
    expect(rows[0].lastTs).toBe(10);
    expect(rows[0].entries.map((x) => x.ts)).toEqual([10, 5]);
  });

  it("not grouped: one row per entry", () => {
    const same = [entry("o0", "optimizer", "info", 1, "stream entry 0"), entry("o1", "optimizer", "info", 2, "stream entry 1")];
    expect(groupTimeline(same, true)).toHaveLength(1);
    expect(groupTimeline(same, false).map((r) => r.text)).toEqual(["stream entry 1", "stream entry 0"]);
  });

  it("keeps the newest entries of a large group, up to the cap", () => {
    const many = Array.from({ length: 60 }, (_, i) => entry(`o${i}`, "optimizer", "info", i, `item ${i}`));
    const [group] = groupTimeline(many, true);
    expect(group.count).toBe(60);
    expect(group.entries).toHaveLength(50);
    expect(group.entries[0].ts).toBe(59);
  });

  it("rows without a readable time sort last", () => {
    const rows = groupTimeline([entry("m0", "module", "warning", null, "no time"), entry("m1", "module", "error", 3, "timed")], true);
    expect(rows.map((r) => r.text)).toEqual(["timed", "no time"]);
  });
});

describe("filterEntries by host", () => {
  const all = [
    entry("m0", "module", "warning", 3, "Fetch of https://www.example.test/a.css failed"),
    entry("m1", "module", "warning", 2, "Fetch of https://example.test/b.css failed"),
    entry("m2", "module", "warning", 1, "no URL at all"),
  ];
  const levels = timelineLevelFilter("debug");

  it("keeps the entries that name the host exactly", () => {
    expect(filterEntries(all, { source: "both", levels, text: "", host: "example.test" }).map((e) => e.id)).toEqual(["m1"]);
    expect(filterEntries(all, { source: "both", levels, text: "", host: "www.example.test" }).map((e) => e.id)).toEqual(["m0"]);
  });

  it("no host: no narrowing", () => {
    expect(filterEntries(all, { source: "both", levels, text: "", host: null })).toHaveLength(3);
  });
});

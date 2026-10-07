// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { EMPTY_MESSAGE_TEXT, MESSAGE_WINDOW_MS, groupForDisplay, groupMessages, messageLevel, toMessageDigest } from "./message-groups";

const NOW = Date.parse("Fri, 02 Oct 2026 10:25:51 GMT");
const at = (time: string) => Date.parse(`Fri, 02 Oct 2026 ${time} GMT`);
const line = (time: string, level: string, text: string) => `[Fri, 02 Oct 2026 ${time} GMT] [${level}] [531] ${text}`;

describe("groupMessages", () => {
  it("groups by severity and template, newest first, and counts the window", () => {
    const messages = [
      { severity: "warning", message: line("10:00:00", "Warning", "Fetch of https://a.test/x.css failed after 12 ms") },
      { severity: "warning", message: line("10:20:00", "Warning", "Fetch of https://b.test/y.css failed after 900 ms") },
      { severity: "error", message: line("10:25:00", "Error", "boom 1") },
      { severity: "info", message: line("09:00:00", "Info", "hello") },
    ];
    expect(groupMessages(messages, NOW, MESSAGE_WINDOW_MS)).toEqual([
      { level: "error", template: "boom N", count: 1, recent: 1, lastMs: at("10:25:00") },
      { level: "warning", template: "Fetch of URL failed after N ms", count: 2, recent: 1, lastMs: at("10:20:00") },
      { level: "info", template: "hello", count: 1, recent: 0, lastMs: at("09:00:00") },
    ]);
  });

  it("gives a continuation line the time of the header line before it", () => {
    const groups = groupMessages(
      [
        { severity: "warning", message: line("10:20:00", "Warning", "head 1") },
        { severity: "warning", message: "  at frame 2" },
      ],
      NOW,
    );
    expect(groups).toEqual([
      { level: "warning", template: "  at frame N", count: 1, recent: 1, lastMs: at("10:20:00") },
      { level: "warning", template: "head N", count: 1, recent: 1, lastMs: at("10:20:00") },
    ]);
  });

  it("a line with no readable time is never recent and has no time", () => {
    expect(
      groupMessages(
        [
          { severity: "warning", message: "  orphan continuation 1" },
          { severity: "warning", message: "[not a time] [Warning] [531] x 1" },
        ],
        NOW,
      ),
    ).toEqual([
      { level: "warning", template: "  orphan continuation N", count: 1, recent: 0, lastMs: 0 },
      { level: "warning", template: "x N", count: 1, recent: 0, lastMs: 0 },
    ]);
  });

  it("skips malformed entries and reads an unknown severity as info", () => {
    const junk: unknown[] = [null, 5, {}, { message: 3 }, { message: "" }, { severity: "bogus", message: "plain" }];
    expect(groupMessages(junk, NOW)).toEqual([{ level: "info", template: "plain", count: 1, recent: 0, lastMs: 0 }]);
    expect(groupMessages("not a list", NOW)).toEqual([]);
  });
});

describe("toMessageDigest", () => {
  it("reads a grouped answer, guarding every field", () => {
    const digest = toMessageDigest(
      {
        scope: "process",
        truncated: true,
        groups: [
          { level: "nope", template: "x", count: "7", last_ms: null },
          { level: "warning", template: "slow N", count: 2, last_ms: 5, recent: 1 },
          { template: 9 },
          null,
          { level: "error", template: "neg", count: -3, last_ms: Number.NaN, recent: Infinity },
        ],
      },
      NOW,
    );
    expect(digest).toEqual({
      source: "module",
      truncated: true,
      groups: [
        { level: "warning", template: "slow N", count: 2, recent: 1, lastMs: 5 },
        { level: "error", template: "neg", count: 0, recent: 0, lastMs: 0 },
        { level: "info", template: "x", count: 0, recent: 0, lastMs: 0 },
      ],
    });
  });

  it("groups a plain answer (an older module) on the console with the same rules", () => {
    const digest = toMessageDigest(
      { scope: "process", next: 2, messages: [{ severity: "warning", message: line("10:20:00", "Warning", "slow 1") }] },
      NOW,
    );
    expect(digest).toEqual({
      source: "console",
      truncated: false,
      groups: [{ level: "warning", template: "slow N", count: 1, recent: 1, lastMs: at("10:20:00") }],
    });
  });

  it("is null for anything else", () => {
    for (const junk of [null, "x", 3, [], {}, { groups: "no", messages: "no" }]) {
      expect(toMessageDigest(junk, NOW), JSON.stringify(junk)).toBeNull();
    }
  });
});

describe("groupForDisplay", () => {
  // Newest first, as the module's messages are kept.
  const items = [
    { severity: "warning", message: line("10:25:51", "Warning", "No permission to rewrite 'https://u.test/s.js'") },
    { severity: "error", message: line("10:25:50", "Error", "Fetch of https://a.test/a.css failed") },
    { severity: "warning", message: line("10:25:49", "Warning", "No permission to rewrite 'https://u.test/s.js?v=2'") },
    { severity: "warning", message: line("10:25:47", "Warning", "No permission to rewrite 'https://u.test/s.js'") },
  ];

  it("folds repeats by severity and template, newest group and newest entry first, without the header", () => {
    const groups = groupForDisplay(items);
    expect(groups.map((g) => [g.severity, g.text, g.count, g.lastTs])).toEqual([
      ["warning", "No permission to rewrite 'https://u.test/s.js'", 3, at("10:25:51")],
      ["error", "Fetch of https://a.test/a.css failed", 1, at("10:25:50")],
    ]);
    expect(groups[0].entries).toEqual([
      { ts: at("10:25:51"), text: "No permission to rewrite 'https://u.test/s.js'" },
      { ts: at("10:25:49"), text: "No permission to rewrite 'https://u.test/s.js?v=2'" },
      { ts: at("10:25:47"), text: "No permission to rewrite 'https://u.test/s.js'" },
    ]);
    expect(new Set(groups.map((g) => g.key)).size).toBe(2);
  });

  it("a group shows the text of its newest line, whatever order the lines come in", () => {
    const groups = groupForDisplay([...items].reverse());
    expect(groups[0].text).toBe("No permission to rewrite 'https://u.test/s.js'");
    expect(groups[0].lastTs).toBe(at("10:25:51"));
  });

  it("keeps at most `cap` entries per group, the newest", () => {
    const many = Array.from({ length: 60 }, (_, i) => ({
      severity: "warning",
      message: line(`10:${String(59 - Math.floor(i / 60)).padStart(2, "0")}:${String(59 - i).padStart(2, "0")}`, "Warning", "slow 1"),
    }));
    const [g] = groupForDisplay(many);
    expect(g.count).toBe(60);
    expect(g.entries).toHaveLength(50);
    expect(g.entries[0].ts).toBe(at("10:59:59"));
  });

  it("an entry without a text message never throws: it is one empty row", () => {
    const junk = [
      { severity: "warning", message: line("10:00:00", "Warning", "slow 1") },
      { severity: "warning", message: null },
      { severity: "warning" },
      { severity: "warning", message: 5 },
      { severity: "warning", message: { text: "x" } },
    ] as unknown as ReadonlyArray<{ severity: string; message: string }>;
    let groups: ReturnType<typeof groupForDisplay> = [];
    expect(() => {
      groups = groupForDisplay(junk);
    }).not.toThrow();
    expect(groups.map((g) => [g.severity, g.text, g.count, g.lastTs])).toEqual([
      ["warning", "slow 1", 1, at("10:00:00")],
      ["warning", EMPTY_MESSAGE_TEXT, 4, null],
    ]);
    expect(groups[1].entries).toEqual(Array.from({ length: 4 }, () => ({ ts: null, text: EMPTY_MESSAGE_TEXT })));
  });

  it("an unknown or missing severity is info", () => {
    const junk = [{ message: "a" }, { severity: 7, message: "a" }] as unknown as ReadonlyArray<{
      severity: string;
      message: string;
    }>;
    expect(groupForDisplay(junk).map((g) => [g.severity, g.count])).toEqual([["info", 2]]);
    expect(messageLevel(undefined)).toBe("info");
    expect(messageLevel("bogus")).toBe("info");
    expect(messageLevel("error")).toBe("error");
  });

  it("a line without a time sorts after timed ones; an unknown severity is info", () => {
    const groups = groupForDisplay([
      { severity: "bogus", message: "  at frame 2" },
      { severity: "warning", message: line("10:00:00", "Warning", "slow 1") },
    ]);
    expect(groups.map((g) => [g.severity, g.lastTs])).toEqual([
      ["warning", at("10:00:00")],
      ["info", null],
    ]);
  });
});

describe("EMPTY_MESSAGE_TEXT", () => {
  it("is the one wording every page and finding shows for an entry without a message", () => {
    expect(EMPTY_MESSAGE_TEXT).toBe("(empty)");
  });
});

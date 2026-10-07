// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it, vi } from "vitest";
import {
  EMPTY_LOG_BUFFER,
  LOGS_PAGE_LIMIT,
  LOG_LEVELS,
  MAX_CATCH_UP_READS,
  MAX_LOG_ROWS,
  RENDER_CAP,
  appendLogPage,
  countUnseen,
  displayLogText,
  filterLogRows,
  formatLogTimestamp,
  globalConsoleLogsHref,
  isKnownLogLevel,
  logsLevelFilter,
  needsCatchUp,
  readLogPages,
  visibleLogRows,
  type LogBuffer,
  type LogEntryRow,
} from "./logs-buffer";

// Pages are built the way the optimizer builds them (its golden file's
// envelope): every key present, keys in the daemon's order.
const entry = (seq: number, extra: Record<string, unknown> = {}) => ({
  level: "info",
  message: `entry ${seq}`,
  module: "worker",
  seq,
  source: "worker",
  timestamp: 1759230000000 + seq * 100,
  type: "log",
  ...extra,
});

const pageOf = (entries: Array<Record<string, unknown>>, extra: Record<string, unknown> = {}) => {
  // Junk entries (null, numbers, strings) are allowed in: some tests send them.
  const seqs = entries.flatMap((e) =>
    e !== null && typeof e === "object" && typeof e.seq === "number" ? [e.seq] : [],
  );
  return {
    entries,
    gap: false,
    more: false,
    newest_seq: seqs.length > 0 ? seqs[seqs.length - 1] : 0,
    next_since: seqs.length > 0 ? seqs[seqs.length - 1] : 0,
    oldest_seq: seqs.length > 0 ? seqs[0] : 0,
    shed_total: 0,
    stream_id: "9f2c4e1a7b3d5c80",
    ...extra,
  };
};

const entryRows = (b: LogBuffer) => b.rows.filter((r): r is LogEntryRow => r.kind === "entry");

describe("constants", () => {
  it("match the design budget", () => {
    expect(MAX_LOG_ROWS).toBe(2000);
    expect(RENDER_CAP).toBe(500);
    expect(LOGS_PAGE_LIMIT).toBe(500);
    expect(MAX_CATCH_UP_READS).toBe(5);
    expect(LOG_LEVELS).toEqual(["error", "warning", "info", "debug"]);
  });
});

describe("appendLogPage", () => {
  it("folds the first page in ascending and adopts its cursor and stream", () => {
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0), entry(1)]));
    expect(entryRows(b).map((r) => r.seq)).toEqual([0, 1]);
    expect(b.nextSince).toBe(1);
    expect(b.streamId).toBe("9f2c4e1a7b3d5c80");
    expect(b.rereadNeeded).toBe(false);
  });

  it("appends later pages after the existing rows", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0), entry(1)]));
    b = appendLogPage(b, pageOf([entry(2), entry(3)], { oldest_seq: 0 }));
    expect(entryRows(b).map((r) => r.seq)).toEqual([0, 1, 2, 3]);
    expect(b.nextSince).toBe(3);
  });

  it("an idle page (the daemon echoes the cursor) returns the same state", () => {
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    const again = appendLogPage(b, pageOf([], { next_since: 0, newest_seq: 0 }));
    expect(again).toBe(b);
  });

  it("a first empty page keeps the cursor unset, so the first entry is not skipped", () => {
    // An untouched ring answers next_since 0; reading since=0 next would
    // skip the entry with seq 0.
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([]));
    expect(b.nextSince).toBeNull();
    b = appendLogPage(b, pageOf([entry(0)]));
    expect(entryRows(b).map((r) => r.seq)).toEqual([0]);
    expect(b.nextSince).toBe(0);
  });

  it("folding the same page twice adds nothing (a racing refresh shares one GET)", () => {
    const p = pageOf([entry(0), entry(1)]);
    const b = appendLogPage(appendLogPage(EMPTY_LOG_BUFFER, p), p);
    expect(entryRows(b)).toHaveLength(2);
    expect(b.nextSince).toBe(1);
  });

  it("drops entries without a usable seq, and anything that is not an entry object", () => {
    const b = appendLogPage(
      EMPTY_LOG_BUFFER,
      pageOf([
        { message: "no seq" },
        entry(4, { seq: "4" }),
        entry(5, { seq: -1 }),
        entry(6, { seq: 1.5 }),
        null as unknown as Record<string, unknown>,
        7 as unknown as Record<string, unknown>,
        "entry" as unknown as Record<string, unknown>,
        entry(8),
      ]),
    );
    expect(entryRows(b).map((r) => r.seq)).toEqual([8]);
  });

  it("anything that is not a page object changes nothing", () => {
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    for (const junk of [null, undefined, 42, "page", [entry(1)]]) {
      expect(appendLogPage(b, junk)).toBe(b);
    }
    expect(appendLogPage(b, { entries: "not a list", next_since: 0 })).toBe(b);
  });

  it("coerces non-string fields and passes attacker-shaped strings through unmodified", () => {
    const hostile = "fetch failed for https://evil.test/<img src=x onerror=alert(1)>?next=javascript:alert(2)";
    const b = appendLogPage(
      EMPTY_LOG_BUFFER,
      pageOf([entry(0, { message: hostile }), entry(1, { message: { odd: true } }), entry(2, { level: 3, timestamp: "soon" })]),
    );
    const rows = entryRows(b);
    expect(rows[0].message).toBe(hostile); // byte-preserved; rendering escapes it
    expect(rows[1].message).toBe('{"odd":true}');
    expect(rows[2].level).toBe("info"); // garbage level normalized
    expect(rows[2].ts).toBe(0); // garbage timestamp normalized
  });

  it("a deeply nested non-string message cannot be stringified; a placeholder shows instead of throwing", () => {
    let nested: unknown[] = [];
    for (let i = 0; i < 100000; i++) nested = [nested];
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0, { message: nested })]));
    expect(entryRows(b)[0].message).toBe("[message not shown]");
  });

  it("precomputes a lowercase haystack of message, source and module", () => {
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0, { message: "Origin SLOW", source: "Cache", module: "Fetch" })]));
    expect(entryRows(b)[0].haystack).toBe("origin slow\ncache\nfetch");
  });

  it("a gap page inserts a dropped-entries marker carrying the lost range", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0), entry(1)]));
    b = appendLogPage(b, pageOf([entry(50), entry(51)], { oldest_seq: 50, gap: true }));
    expect(b.rows.map((r) => r.kind)).toEqual(["entry", "entry", "gap", "entry", "entry"]);
    const marker = b.rows[2];
    expect(marker.kind === "gap" && marker.fromSeq).toBe(1);
    expect(marker.kind === "gap" && marker.toSeq).toBe(50);
    expect(b.nextSince).toBe(51);
  });

  it("a first page with gap set inserts no marker (no cursor was lost yet)", () => {
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(50)], { oldest_seq: 50, gap: true }));
    expect(b.rows.map((r) => r.kind)).toEqual(["entry"]);
  });

  it("a changed stream_id on an empty echo page is a restart: marker, cursor reset, re-read", () => {
    // Exactly what a restarted optimizer answers a cursor from its previous
    // process: no entries, next_since echoing the cursor, a new stream_id.
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(100), entry(101)]));
    const echo = pageOf([], { next_since: 101, newest_seq: 1, oldest_seq: 0, stream_id: "3b7d0e5f1a2c4968" });
    b = appendLogPage(b, echo);
    expect(b.rows.map((r) => r.kind)).toEqual(["entry", "entry", "restart"]);
    expect(b.nextSince).toBeNull();
    expect(b.streamId).toBe("3b7d0e5f1a2c4968");
    expect(b.rereadNeeded).toBe(true);
    expect(needsCatchUp(echo, b)).toBe(true);
    // The re-read takes the new process's newest page; nothing is deduped
    // against the old cursor.
    b = appendLogPage(b, pageOf([entry(0), entry(1), entry(2)], { stream_id: "3b7d0e5f1a2c4968" }));
    expect(entryRows(b).map((r) => r.seq)).toEqual([100, 101, 0, 1, 2]);
    expect(b.rereadNeeded).toBe(false);
    expect(b.nextSince).toBe(2);
  });

  it("a restart page's own entries are dropped (they answer the old cursor)", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(2), entry(3)]));
    b = appendLogPage(b, pageOf([entry(4), entry(5)], { stream_id: "3b7d0e5f1a2c4968" }));
    expect(entryRows(b).map((r) => r.seq)).toEqual([2, 3]);
    expect(b.rows[b.rows.length - 1].kind).toBe("restart");
  });

  it("row ids stay unique across a restart (daemon seqs repeat)", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0), entry(1)]));
    b = appendLogPage(b, pageOf([], { next_since: 1, stream_id: "3b7d0e5f1a2c4968" }));
    b = appendLogPage(b, pageOf([entry(0), entry(1)], { stream_id: "3b7d0e5f1a2c4968" }));
    expect(new Set(b.rows.map((r) => r.id)).size).toBe(b.rows.length);
  });

  it("a rising shed_total inserts a marker with the count; the first page only adopts it", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)], { shed_total: 7 }));
    expect(b.rows.map((r) => r.kind)).toEqual(["entry"]);
    b = appendLogPage(b, pageOf([entry(1)], { shed_total: 19 }));
    expect(b.rows.map((r) => r.kind)).toEqual(["entry", "shed", "entry"]);
    const marker = b.rows[1];
    expect(marker.kind === "shed" && marker.count).toBe(12);
    expect(b.shedTotal).toBe(19);
  });

  it("drops the oldest rows past the cap, markers included", () => {
    let b = EMPTY_LOG_BUFFER;
    for (let p = 0; p < 5; p++) {
      b = appendLogPage(b, pageOf([entry(p * 2), entry(p * 2 + 1)]), 4);
    }
    expect(b.rows).toHaveLength(4);
    expect(entryRows(b).map((r) => r.seq)).toEqual([6, 7, 8, 9]);
  });

  it("a garbage next_since falls back to the last folded entry", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    b = appendLogPage(b, pageOf([entry(1)], { next_since: "soon" }));
    expect(b.nextSince).toBe(1);
    expect(entryRows(b).map((r) => r.seq)).toEqual([0, 1]);
  });

  it("the cursor never moves backwards within one stream", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(5)]));
    b = appendLogPage(b, pageOf([], { next_since: 2 }));
    expect(b.nextSince).toBe(5);
  });
});

describe("needsCatchUp", () => {
  it("is true on more, on a full page with a cursor, and after a restart", () => {
    const b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    expect(needsCatchUp(pageOf([entry(1)], { more: true }), b)).toBe(true);
    const full = pageOf(Array.from({ length: LOGS_PAGE_LIMIT }, (_, i) => entry(i + 1)));
    expect(needsCatchUp(full, b)).toBe(true);
    expect(needsCatchUp(pageOf([entry(1)]), b)).toBe(false);
    expect(needsCatchUp(null, b)).toBe(false);
    expect(needsCatchUp(pageOf([]), { ...b, rereadNeeded: true })).toBe(true);
  });
});

describe("readLogPages", () => {
  // A reader that answers the given pages in order and records its cursors.
  const reader = (pages: unknown[]) => {
    const calls: Array<number | undefined> = [];
    const read = vi.fn(async (since?: number) => {
      calls.push(since);
      const next = pages.shift();
      if (next instanceof Error) throw next;
      return next;
    });
    return { read, calls };
  };

  it("reads once when the answer is complete", async () => {
    const { read, calls } = reader([pageOf([entry(0), entry(1)])]);
    const out = await readLogPages(read, EMPTY_LOG_BUFFER);
    expect(out.reads).toBe(1);
    expect(calls).toEqual([undefined]);
    expect(entryRows(out.state).map((r) => r.seq)).toEqual([0, 1]);
  });

  it("catches up while the optimizer says more, passing each new cursor", async () => {
    const { read, calls } = reader([
      pageOf([entry(1), entry(2)], { more: true }),
      pageOf([entry(3), entry(4)], { more: true }),
      pageOf([entry(5)]),
    ]);
    const start = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    const out = await readLogPages(read, start);
    expect(calls).toEqual([0, 2, 4]);
    expect(out.reads).toBe(3);
    expect(entryRows(out.state).map((r) => r.seq)).toEqual([0, 1, 2, 3, 4, 5]);
  });

  it("stops after MAX_CATCH_UP_READS extra reads; the next poll continues", async () => {
    const pages = Array.from({ length: 10 }, (_, i) => pageOf([entry(i + 1)], { more: true }));
    const { read } = reader(pages);
    const start = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    const out = await readLogPages(read, start);
    expect(read).toHaveBeenCalledTimes(1 + MAX_CATCH_UP_READS);
    expect(out.state.nextSince).toBe(1 + MAX_CATCH_UP_READS);
  });

  it("the first read's failure is the poll's failure (a busy 429 included)", async () => {
    const busy = new Error("HTTP 429");
    const { read } = reader([busy]);
    await expect(readLogPages(read, EMPTY_LOG_BUFFER)).rejects.toBe(busy);
  });

  it("a failed catch-up read ends the poll quietly with what it has", async () => {
    const { read } = reader([pageOf([entry(0), entry(1)], { more: true }), new Error("HTTP 429")]);
    const out = await readLogPages(read, EMPTY_LOG_BUFFER);
    expect(read).toHaveBeenCalledTimes(2);
    expect(entryRows(out.state).map((r) => r.seq)).toEqual([0, 1]);
    expect(out.state.nextSince).toBe(1);
  });

  it("a catch-up page that cannot be folded ends the poll like a failed read does", async () => {
    // Simulates a fold failure independent of any one cause: a raw entry
    // whose field access itself throws, so the throw escapes appendLogPage
    // before any defensive coercion inside it runs.
    const poison: Record<string, unknown> = {};
    Object.defineProperty(poison, "seq", {
      get(): number {
        throw new Error("boom");
      },
    });
    const badPage = {
      entries: [poison],
      gap: false,
      more: false,
      next_since: 2,
      oldest_seq: 0,
      shed_total: 0,
      stream_id: "9f2c4e1a7b3d5c80", // same stream as pageOf()'s default: a normal page, not a restart
    };
    const { read } = reader([pageOf([entry(0), entry(1)], { more: true }), badPage]);
    const out = await readLogPages(read, EMPTY_LOG_BUFFER);
    expect(read).toHaveBeenCalledTimes(2);
    expect(entryRows(out.state).map((r) => r.seq)).toEqual([0, 1]);
    expect(out.state.nextSince).toBe(1);
  });

  it("after a restart it re-reads the newest page at once", async () => {
    const start = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(100)]));
    const { read, calls } = reader([
      pageOf([], { next_since: 100, stream_id: "3b7d0e5f1a2c4968" }),
      pageOf([entry(0), entry(1)], { stream_id: "3b7d0e5f1a2c4968" }),
    ]);
    const out = await readLogPages(read, start);
    expect(calls).toEqual([100, undefined]);
    expect(out.state.rows.map((r) => r.kind)).toEqual(["entry", "restart", "entry", "entry"]);
  });
});

describe("filterLogRows", () => {
  const all = { error: true, warning: true, info: true, debug: true };
  const buffer = appendLogPage(
    EMPTY_LOG_BUFFER,
    pageOf([
      entry(0, { level: "error", message: "analysis timed out", source: "chrome", module: "browser" }),
      entry(1, { level: "warning", message: "origin fetch slow", source: "cache", module: "cache" }),
      entry(2, { level: "info", message: "cache flush complete" }),
      entry(3, { level: "trace", message: "a level this console does not know" }),
    ]),
  );

  it("level toggles hide known levels; an unknown level is always shown", () => {
    const out = filterLogRows(buffer.rows, { error: false, warning: false, info: false, debug: false }, "");
    expect(out.map((r) => r.kind === "entry" && r.message)).toEqual(["a level this console does not know"]);
  });

  it("text matches message, source and module, case-insensitively", () => {
    expect(filterLogRows(buffer.rows, all, "SLOW")).toHaveLength(1);
    expect(filterLogRows(buffer.rows, all, "chrome")).toHaveLength(1);
    expect(filterLogRows(buffer.rows, all, "browser")).toHaveLength(1);
    expect(filterLogRows(buffer.rows, all, "  zzz ")).toHaveLength(0);
  });

  it("markers pass every filter (a hole in the stream is always visible)", () => {
    let b = appendLogPage(EMPTY_LOG_BUFFER, pageOf([entry(0)]));
    b = appendLogPage(b, pageOf([entry(50)], { oldest_seq: 50, gap: true }));
    const out = filterLogRows(b.rows, { error: false, warning: false, info: false, debug: false }, "no match");
    expect(out.map((r) => r.kind)).toEqual(["gap"]);
  });
});

describe("visibleLogRows / countUnseen (the render window)", () => {
  let b = EMPTY_LOG_BUFFER;
  for (let p = 0; p < 10; p++) b = appendLogPage(b, pageOf([entry(p)]));
  const rows = b.rows; // ids 0..9

  it("following: the newest `cap` rows", () => {
    expect(visibleLogRows(rows, null, 3).map((r) => r.id)).toEqual([7, 8, 9]);
  });

  it("not following: the window stays at its anchor while rows arrive", () => {
    expect(visibleLogRows(rows, 4, 3).map((r) => r.id)).toEqual([4, 5, 6]);
    // An anchor that was evicted falls back to the oldest remaining rows.
    expect(visibleLogRows(rows.slice(6), 4, 3).map((r) => r.id)).toEqual([6, 7, 8]);
  });

  it("counts only entry rows that arrived since the reader scrolled away and pass the filters", () => {
    expect(countUnseen(rows, null)).toBe(0);
    expect(countUnseen(rows, 7)).toBe(3);
    expect(countUnseen(filterLogRows(rows, { error: true, warning: true, info: true, debug: true }, "entry 9"), 7)).toBe(1);
  });
});

describe("logsLevelFilter / isKnownLogLevel (the ?level= preset)", () => {
  it("shows the named level and anything more severe; absent or unknown shows all", () => {
    expect(logsLevelFilter("error")).toEqual({ error: true, warning: false, info: false, debug: false });
    expect(logsLevelFilter("warning")).toEqual({ error: true, warning: true, info: false, debug: false });
    expect(logsLevelFilter(null)).toEqual({ error: true, warning: true, info: true, debug: true });
    expect(logsLevelFilter("bogus")).toEqual({ error: true, warning: true, info: true, debug: true });
    expect(isKnownLogLevel("debug")).toBe(true);
    expect(isKnownLogLevel("fatal")).toBe(false); // the module's vocabulary, not the optimizer's
    expect(isKnownLogLevel("__proto__")).toBe(false);
    expect(isKnownLogLevel(null)).toBe(false);
  });
});

describe("formatLogTimestamp", () => {
  it("formats the local date and time to the millisecond, padded", () => {
    const ts = new Date(2026, 0, 1, 1, 2, 3, 4).getTime();
    expect(formatLogTimestamp(ts)).toBe("2026-01-01 01:02:03.004");
    expect(formatLogTimestamp(1759230000000)).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}$/);
  });

  it("renders garbage as a dash", () => {
    expect(formatLogTimestamp(0)).toBe("—");
    expect(formatLogTimestamp(NaN)).toBe("—");
  });

  it("renders an out-of-range timestamp as a dash, never NaN", () => {
    expect(formatLogTimestamp(1e16)).toBe("—");
  });
});

describe("displayLogText", () => {
  it("shows LF, CR, ESC and DEL as visible glyphs; TAB and a bidi override pass through unchanged", () => {
    expect(displayLogText("a\nb")).toBe("a⏎b"); // LF -> ⏎
    expect(displayLogText("a\rb")).toBe("a␍b"); // CR -> ␍
    expect(displayLogText("a\x1bb")).toBe("a␛b"); // ESC -> control picture
    expect(displayLogText("a\x7fb")).toBe("a␡b"); // DEL -> its control picture
    expect(displayLogText("a\tb")).toBe("a\tb"); // TAB stays a tab
    expect(displayLogText("a‮b")).toBe("a‮b"); // bidi override left as is
  });

  it("leaves ordinary text byte for byte", () => {
    const plain = "GET /heavy.png 200 1024 https://example.test/a?b=c";
    expect(displayLogText(plain)).toBe(plain);
  });
});

describe("globalConsoleLogsHref", () => {
  it("links from any per-vhost mount, keeping a known level preset", () => {
    expect(globalConsoleLogsHref("/pagespeed_admin", null)).toBe("/pagespeed_global_admin/#/logs");
    expect(globalConsoleLogsHref("/pagespeed_admin", "error")).toBe("/pagespeed_global_admin/#/logs?level=error");
    expect(globalConsoleLogsHref("/site/pagespeed_admin", "bogus")).toBe("/site/pagespeed_global_admin/#/logs");
    expect(globalConsoleLogsHref("/pagespeed_global_admin", null)).toBeNull();
    expect(globalConsoleLogsHref("/renamed_admin", "error")).toBeNull();
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from "vitest";
import { mergeMessages, messageBody } from "./message-cursor";

const m = (s: string) => ({ severity: "info", message: s });

describe("mergeMessages", () => {
  it("appends new messages and keeps the newest first", () => {
    const state = { items: [m("old")], next: 5 };
    const out = mergeMessages(state, { scope: "process", next: 6, messages: [m("new")] }, 1000);
    expect(out.items.map((x) => x.message)).toEqual(["new", "old"]);
    expect(out.next).toBe(6);
  });

  it("replaces everything when the backend restarted (next went backwards)", () => {
    const out = mergeMessages(
      { items: [m("old")], next: 50 },
      { scope: "process", next: 2, messages: [m("fresh")] },
      1000,
    );
    expect(out.items.map((x) => x.message)).toEqual(["fresh"]);
  });

  it("caps the retained list", () => {
    const state = { items: Array.from({ length: 1000 }, (_, i) => m(`x${i}`)), next: 1000 };
    const out = mergeMessages(state, { scope: "process", next: 1001, messages: [m("y")] }, 1000);
    expect(out.items).toHaveLength(1000);
    expect(out.items[0].message).toBe("y");
  });

  it("is idempotent when the same page is merged twice (poll racing a manual refresh)", () => {
    const state = { items: [m("old")], next: 5 };
    const page = { scope: "process", next: 6, messages: [m("new")] };
    const once = mergeMessages(state, page, 1000);
    const twice = mergeMessages(once, page, 1000);
    expect(twice.items.map((x) => x.message)).toEqual(["new", "old"]);
    expect(twice.next).toBe(6);
  });
});

describe("messageBody", () => {
  it("drops the module's leading date prefix", () => {
    expect(messageBody("[Sat, 26 Sep 2026 10:02:31 GMT] [Info] [531] CycloneCache enabled")).toBe(
      "[Info] [531] CycloneCache enabled",
    );
  });
  it("leaves text without a date prefix alone", () => {
    expect(messageBody("[Info] no date here")).toBe("[Info] no date here");
    expect(messageBody("plain text")).toBe("plain text");
  });
});

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { refreshResultText, updatedText } from "./page-header";

describe("updatedText", () => {
  const now = 1_800_000_000_000;
  it("is a relative stamp while auto-refreshing", () => {
    expect(updatedText(now - 12_000, now, true)).toBe("updated 12 s ago");
    expect(updatedText(now - 5 * 60_000, now, true)).toBe("updated 5 min ago");
  });
  it("says so when paused", () => {
    expect(updatedText(now - 12_000, now, false)).toBe("paused · updated 12 s ago");
  });
  it("counts as time passes", () => {
    const at = now - 2_000;
    expect(updatedText(at, now, true, 5000)).toBe("updated 2 s ago");
    expect(updatedText(at, now + 1_000, true, 5000)).toBe("updated 3 s ago");
    expect(updatedText(at, now + 9_000, true, 5000)).toBe("updated 11 s ago");
  });
  it("says a refresh is overdue once the stamp has missed three intervals", () => {
    expect(updatedText(now - 15_000, now, true, 5000)).toBe("updated 15 s ago");
    expect(updatedText(now - 15_001, now, true, 5000)).toBe("refresh overdue · updated 15 s ago");
    expect(updatedText(now - 2 * 60_000, now, true, 5000)).toBe("refresh overdue · updated 2 min ago");
    // A slower page is not overdue at the same age.
    expect(updatedText(now - 60_000, now, true, 30_000)).toBe("updated 1 min ago");
    expect(updatedText(now - 91_000, now, true, 30_000)).toBe("refresh overdue · updated 1 min ago");
  });
  it("is never overdue while paused or without an interval", () => {
    expect(updatedText(now - 10 * 60_000, now, false, 5000)).toBe("paused · updated 10 min ago");
    expect(updatedText(now - 10 * 60_000, now, true)).toBe("updated 10 min ago");
  });
  it("admits it has never updated", () => {
    expect(updatedText(null, now, true)).toBe("updated —");
    expect(updatedText(null, now, false)).toBe("updated —");
  });
});

describe("refreshResultText", () => {
  it("says how a refresh the viewer asked for ended", () => {
    expect(refreshResultText("ok")).toBe("Refreshed");
    expect(refreshResultText("failed")).toBe("Refresh failed");
    expect(refreshResultText("busy")).toBe("Server busy; not refreshed");
    // A leaf turned off answered; a page refresh without an outcome is an answer.
    expect(refreshResultText("off")).toBe("Refreshed");
    expect(refreshResultText(undefined)).toBe("Refreshed");
  });
});

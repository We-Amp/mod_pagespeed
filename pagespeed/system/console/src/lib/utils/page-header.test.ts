// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { updatedText } from "./page-header";

describe("updatedText", () => {
  const now = 1_800_000_000_000;
  it("is a relative stamp while auto-refreshing", () => {
    expect(updatedText(now - 12_000, now, true)).toBe("updated 12 s ago");
    expect(updatedText(now - 5 * 60_000, now, true)).toBe("updated 5 min ago");
  });
  it("says so when paused", () => {
    expect(updatedText(now - 12_000, now, false)).toBe("paused · updated 12 s ago");
  });
  it("admits it has never updated", () => {
    expect(updatedText(null, now, true)).toBe("updated —");
    expect(updatedText(null, now, false)).toBe("updated —");
  });
});

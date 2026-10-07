// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { nextTabId } from "./tabs";

const IDS = ["structure", "lookup", "purge"];

describe("nextTabId", () => {
  it("arrow keys move and wrap", () => {
    expect(nextTabId(IDS, "structure", "ArrowRight")).toBe("lookup");
    expect(nextTabId(IDS, "purge", "ArrowRight")).toBe("structure");
    expect(nextTabId(IDS, "structure", "ArrowLeft")).toBe("purge");
    expect(nextTabId(IDS, "lookup", "ArrowDown")).toBe("purge");
    expect(nextTabId(IDS, "lookup", "ArrowUp")).toBe("structure");
  });
  it("Home and End jump to the ends", () => {
    expect(nextTabId(IDS, "lookup", "Home")).toBe("structure");
    expect(nextTabId(IDS, "lookup", "End")).toBe("purge");
  });
  it("other keys, or an unknown current tab, do nothing", () => {
    expect(nextTabId(IDS, "lookup", "Enter")).toBeNull();
    expect(nextTabId(IDS, "missing", "ArrowRight")).toBeNull();
    expect(nextTabId([], "x", "ArrowRight")).toBeNull();
  });
});

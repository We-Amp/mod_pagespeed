// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { scopeChipShown } from "./scope-chip";

describe("scopeChipShown", () => {
  it("shows nothing on the whole-server console without a lens", () => {
    expect(scopeChipShown(true, true, false)).toBe(false);
  });

  it("marks the whole-server cards under a host lens", () => {
    expect(scopeChipShown(true, true, true)).toBe(true);
  });

  it("marks only the whole-server cards on a per-host console", () => {
    expect(scopeChipShown(true, false, false)).toBe(true);
    expect(scopeChipShown(false, false, false)).toBe(false);
  });

  it("never marks a card about the host the console covers", () => {
    expect(scopeChipShown(false, false, true)).toBe(false);
    expect(scopeChipShown(false, true, true)).toBe(false);
  });
});

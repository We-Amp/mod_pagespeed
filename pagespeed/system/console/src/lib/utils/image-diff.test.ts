// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { sliderKeyTarget, sliderValueText } from "./image-diff";

describe("sliderKeyTarget", () => {
  it("steps by 5 with the arrows and jumps with Home/End", () => {
    expect(sliderKeyTarget("ArrowRight", 50)).toBe(55);
    expect(sliderKeyTarget("ArrowUp", 50)).toBe(55);
    expect(sliderKeyTarget("ArrowLeft", 50)).toBe(45);
    expect(sliderKeyTarget("ArrowDown", 50)).toBe(45);
    expect(sliderKeyTarget("Home", 50)).toBe(0);
    expect(sliderKeyTarget("End", 50)).toBe(100);
  });
  it("clamps to 0..100 and ignores other keys", () => {
    expect(sliderKeyTarget("ArrowRight", 98)).toBe(100);
    expect(sliderKeyTarget("ArrowLeft", 2)).toBe(0);
    expect(sliderKeyTarget("Enter", 50)).toBeNull();
    expect(sliderKeyTarget("a", 50)).toBeNull();
  });
});

describe("sliderValueText", () => {
  it("says how much of the original shows", () => {
    expect(sliderValueText(45)).toBe("45% original");
    expect(sliderValueText(33.4)).toBe("33% original");
  });
});

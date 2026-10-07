// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { DaemonAlternate } from "$lib/api/types";
import {
  findMatchingOriginal,
  formatSavingsPercent,
  formatToMimeType,
  qualityTier,
  qualityTierLabel,
  savingsPercent,
} from "./quality";

describe("qualityTier", () => {
  it("classifies at the documented boundaries", () => {
    expect(qualityTier(70)).toBe("good");
    expect(qualityTier(69.9)).toBe("acceptable");
    expect(qualityTier(50)).toBe("acceptable");
    expect(qualityTier(49.9)).toBe("poor");
  });
  it("labels every tier", () => {
    for (const tier of ["good", "acceptable", "poor"] as const) {
      expect(qualityTierLabel(tier).length).toBeGreaterThan(0);
    }
  });
});

describe("savingsPercent", () => {
  it("computes the savings and never leaves [0, 100]", () => {
    expect(savingsPercent(100, 20)).toBe(80);
    expect(savingsPercent(0, 20)).toBe(0);
    expect(savingsPercent(100, 0)).toBe(100);
    // An optimized variant LARGER than the original (incompressible
    // content) reports 0%, not a negative number.
    expect(savingsPercent(100, 150)).toBe(0);
  });
});

describe("formatSavingsPercent", () => {
  it("formats with the minus sign, or 0%", () => {
    expect(formatSavingsPercent(100, 20)).toBe("-80.0%");
    expect(formatSavingsPercent(100, 100)).toBe("0%");
    expect(formatSavingsPercent(100, 150)).toBe("0%");
  });
});

describe("formatToMimeType", () => {
  it("maps formats and falls back to the origin content type", () => {
    expect(formatToMimeType("webp")).toBe("image/webp");
    expect(formatToMimeType("avif")).toBe("image/avif");
    expect(formatToMimeType("original", "image/png")).toBe("image/png");
    expect(formatToMimeType("original")).toBe("unknown");
    expect(formatToMimeType("jpeg", "image/jpeg")).toBe("image/jpeg");
    expect(formatToMimeType("png")).toBe("png");
  });
});

describe("findMatchingOriginal", () => {
  const original = (
    over: Partial<DaemonAlternate> = {},
  ): DaemonAlternate => ({
    alternate_id: 0,
    format: "original",
    viewport: "desktop",
    density: "1x",
    save_data: false,
    is_sentinel: false,
    ...over,
  });
  it("matches viewport, density and save-data", () => {
    const variant = original({ alternate_id: 1, format: "webp" });
    expect(findMatchingOriginal(variant, [original(), variant])).toEqual(
      original(),
    );
  });
  it("does not match a different viewport or density", () => {
    const variant = original({ alternate_id: 1, format: "webp", viewport: "mobile" });
    expect(findMatchingOriginal(variant, [original()])).toBeUndefined();
    const dense = original({ alternate_id: 2, format: "webp", density: "2x+" });
    expect(findMatchingOriginal(dense, [original()])).toBeUndefined();
  });
  it("never returns a sentinel or a non-original (a worker-recompressed \"jpeg\" is a variant)", () => {
    const variant = original({ alternate_id: 1, format: "webp" });
    expect(
      findMatchingOriginal(variant, [
        { is_sentinel: true, sentinel_name: "content_hash" },
        original({ format: "webp" }),
        original({ format: "jpeg" }),
      ]),
    ).toBeUndefined();
  });
});

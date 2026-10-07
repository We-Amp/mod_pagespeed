// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Variant quality math, ported from the 2.0 dashboard.  Badge colors are
 * NOT ported: they live in the badge components, on this console's theme
 * tokens.
 */

import type { DaemonAlternate } from "$lib/api/types";

export type QualityTier = "good" | "acceptable" | "poor";

/** Classify an SSIMULACRA2 score into a quality tier. */
export function qualityTier(score: number): QualityTier {
  if (score >= 70) return "good";
  if (score >= 50) return "acceptable";
  return "poor";
}

/** Human-readable label for a quality tier. */
export function qualityTierLabel(tier: QualityTier): string {
  switch (tier) {
    case "good":
      return "Good quality — perceptually near-lossless";
    case "acceptable":
      return "Acceptable quality — minor artifacts may be visible";
    case "poor":
      return "Poor quality — noticeable degradation";
  }
}

/**
 * Savings as a percentage, clamped to [0, 100]: an optimized variant
 * larger than the original (incompressible content) reports 0, never a
 * negative number.
 */
export function savingsPercent(originalSize: number, optimizedSize: number): number {
  if (originalSize <= 0) return 0;
  const pct = ((originalSize - optimizedSize) / originalSize) * 100;
  return Math.min(100, Math.max(0, pct));
}

/** Format a savings percentage (e.g. "-84.5%", "0%"). Never "+". */
export function formatSavingsPercent(originalSize: number, optimizedSize: number): string {
  const pct = savingsPercent(originalSize, optimizedSize);
  return pct === 0 ? "0%" : `-${pct.toFixed(1)}%`;
}

/** Derive the actual MIME type from a variant's format field. */
export function formatToMimeType(format: string, originContentType?: string): string {
  switch (format) {
    case "webp":
      return "image/webp";
    case "avif":
      return "image/avif";
    case "svg":
      return "image/svg+xml";
    case "original":
      return originContentType || "unknown";
    default:
      return originContentType || format || "unknown";
  }
}

/**
 * The original-format variant matching the given variant's viewport class,
 * pixel density and save-data setting; undefined when no matching original
 * exists.
 */
export function findMatchingOriginal(
  alt: DaemonAlternate,
  alternates: DaemonAlternate[],
): DaemonAlternate | undefined {
  return alternates.find(
    (a) =>
      a.format === "original" &&
      a.is_sentinel !== true &&
      a.viewport === alt.viewport &&
      a.density === alt.density &&
      a.save_data === alt.save_data,
  );
}

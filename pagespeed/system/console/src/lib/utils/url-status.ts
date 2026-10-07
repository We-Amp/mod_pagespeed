// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * URL optimization status, computed from a URL's cached variants.
 * Ported from the 2.0 dashboard onto the daemon's own field vocabulary
 * (content_type is a class; brotli is spelled "brotli").
 */

import type { DaemonAlternate } from "$lib/api/types";

export type UrlStatus = "complete" | "partial" | "original-only" | "revalidating";

/**
 * - `complete`       — full variant coverage (images: modern + AVIF; text: gzip + brotli)
 * - `partial`        — some variants but not the full set
 * - `original-only`  — only the original variant exists
 * - `revalidating`   — at least one variant is flagged for revalidation
 * - `null`           — no non-sentinel variants (nothing to classify)
 */
export function computeUrlStatus(alternates: DaemonAlternate[]): UrlStatus | null {
  const nonSentinels = alternates.filter((a) => a.is_sentinel !== true);
  if (nonSentinels.length === 0) return null;
  if (nonSentinels.some((a) => a.needs_revalidation === true)) return "revalidating";
  if (nonSentinels.length === 1) return "original-only";

  const formats = new Set(nonSentinels.map((a) => a.format));
  const encodings = new Set(nonSentinels.map((a) => a.encoding));
  const hasModernImage = formats.has("webp") || formats.has("avif") || formats.has("svg");
  const hasRaster = formats.has("jpeg") || formats.has("png") || formats.has("gif");
  // Content is image if it has image-specific formats, raster codec names,
  // or the "image" content class. "original" format alone is NOT
  // sufficient — HTML/CSS/JS variants also use format "original".
  const isImageContent =
    hasModernImage || hasRaster || nonSentinels.some((a) => a.content_type === "image");

  if (isImageContent && hasModernImage && formats.has("avif")) {
    return "complete";
  }
  if (!isImageContent && encodings.has("gzip") && encodings.has("brotli")) {
    return "complete";
  }
  return "partial";
}

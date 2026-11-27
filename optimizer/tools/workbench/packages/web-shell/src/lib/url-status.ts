// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// URL optimization status computation.
// Extracted from the URLs page for testability.
// ---------------------------------------------------------------------------

import type { AlternateInfo } from '@pagespeed/api-client';

export type UrlStatus = 'complete' | 'partial' | 'original-only' | 'revalidating';

/**
 * Determine the optimization status of a cached URL based on its alternates.
 *
 * - `complete`       — full variant coverage (images: has AVIF+modern; text: gzip+brotli)
 * - `partial`        — worker has produced some variants but not the full set
 * - `original-only`  — only the nginx-written original alternate exists
 * - `revalidating`   — at least one alternate is flagged for revalidation
 * - `null`           — no non-sentinel alternates (nothing to classify)
 */
export function computeUrlStatus(alts: AlternateInfo[]): UrlStatus | null {
  const nonSentinels = alts.filter((a) => !a.is_sentinel);
  if (nonSentinels.length === 0) return null;
  if (nonSentinels.some((a) => a.needs_revalidation)) return 'revalidating';
  if (nonSentinels.length === 1) return 'original-only';

  // Check for expected variant coverage.
  const formats = new Set(nonSentinels.map((a) => a.format));
  const encodings = new Set(nonSentinels.map((a) => a.encoding));
  const hasModernImage = formats.has('webp') || formats.has('avif') || formats.has('svg');
  const hasRaster = formats.has('jpeg') || formats.has('png') || formats.has('gif');
  // Content is image if it has image-specific formats, raster codec names,
  // or an "image" content type. "original" format alone is NOT sufficient —
  // HTML/CSS/JS variants also use format "original".
  const isImageContent =
    hasModernImage ||
    hasRaster ||
    nonSentinels.some((a) => a.content_type === 'image');

  if (isImageContent && hasModernImage && formats.has('avif')) {
    return 'complete';
  }
  if (!isImageContent && encodings.has('gzip') && encodings.has('brotli')) {
    return 'complete';
  }
  return 'partial';
}

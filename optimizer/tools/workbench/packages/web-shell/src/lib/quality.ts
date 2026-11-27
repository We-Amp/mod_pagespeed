// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export type QualityTier = 'good' | 'acceptable' | 'poor';

/** Classify an SSIMULACRA2 score into a quality tier. */
export function qualityTier(score: number): QualityTier {
  if (score >= 70) return 'good';
  if (score >= 50) return 'acceptable';
  return 'poor';
}

/** Human-readable label for a quality tier. */
export function qualityTierLabel(tier: QualityTier): string {
  switch (tier) {
    case 'good':
      return 'Good quality — perceptually near-lossless';
    case 'acceptable':
      return 'Acceptable quality — minor artifacts may be visible';
    case 'poor':
      return 'Poor quality — noticeable degradation';
  }
}

/** Compute savings percentage (0-100). Returns 0 if original is 0. */
export function savingsPercent(
  originalSize: number,
  optimizedSize: number,
): number {
  if (originalSize <= 0) return 0;
  return ((originalSize - optimizedSize) / originalSize) * 100;
}

/** Format a savings percentage with sign (e.g. "-84.5%", "+12.3%"). */
export function formatSavingsPercent(
  originalSize: number,
  optimizedSize: number,
): string {
  const pct = savingsPercent(originalSize, optimizedSize);
  if (pct === 0) return '0%';
  const sign = pct > 0 ? '-' : '+';
  return `${sign}${Math.abs(pct).toFixed(1)}%`;
}

/** CSS custom property name for a content class badge color. */
export function contentClassColor(
  contentClass: string,
): { bg: string; fg: string } {
  switch (contentClass) {
    case 'photo':
      return { bg: 'var(--ps-quality-photo-bg)', fg: 'var(--ps-quality-photo-fg)' };
    case 'screenshot':
      return { bg: 'var(--ps-quality-screenshot-bg)', fg: 'var(--ps-quality-screenshot-fg)' };
    case 'illustration':
      return { bg: 'var(--ps-quality-illustration-bg)', fg: 'var(--ps-quality-illustration-fg)' };
    case 'noisy':
      return { bg: 'var(--ps-quality-noisy-bg)', fg: 'var(--ps-quality-noisy-fg)' };
    default:
      return { bg: 'var(--ps-bg-tertiary)', fg: 'var(--ps-fg-secondary)' };
  }
}

/** CSS custom property name for a format badge color. */
export function formatBadgeColor(
  format: string,
): { bg: string; fg: string } {
  switch (format.toLowerCase()) {
    case 'webp':
      return { bg: 'var(--ps-quality-webp-bg)', fg: 'var(--ps-quality-webp-fg)' };
    case 'avif':
      return { bg: 'var(--ps-quality-avif-bg)', fg: 'var(--ps-quality-avif-fg)' };
    case 'jpeg':
    case 'jpg':
      return { bg: 'var(--ps-quality-jpeg-bg)', fg: 'var(--ps-quality-jpeg-fg)' };
    case 'png':
      return { bg: 'var(--ps-quality-png-bg)', fg: 'var(--ps-quality-png-fg)' };
    default:
      return { bg: 'var(--ps-bg-tertiary)', fg: 'var(--ps-fg-secondary)' };
  }
}

/** Derive actual MIME type from the alternate's format field. */
export function formatToMimeType(
  format: string,
  originContentType?: string,
): string {
  switch (format) {
    case 'webp':
      return 'image/webp';
    case 'avif':
      return 'image/avif';
    case 'svg':
      return 'image/svg+xml';
    case 'original':
      return originContentType || 'unknown';
    default:
      return originContentType || format || 'unknown';
  }
}

/**
 * Find the original-format alternate that matches the same viewport class,
 * pixel density, and save-data setting as the given alternate.  Returns
 * undefined if no matching original exists.
 */
export function findMatchingOriginal<
  T extends { format?: string; viewport?: string; density?: string; save_data?: boolean; is_sentinel?: boolean },
>(alt: T, alternates: T[]): T | undefined {
  return alternates.find(
    (a) =>
      a.format === 'original' &&
      !a.is_sentinel &&
      a.viewport === alt.viewport &&
      a.density === alt.density &&
      a.save_data === alt.save_data,
  );
}

/** Summary statistics for a set of SSIMULACRA2 scores. */
export interface QualityDistribution {
  good: number;
  acceptable: number;
  poor: number;
  min: number;
  max: number;
  avg: number;
  total: number;
}

/** Compute quality distribution from a list of scores. */
export function computeQualityDistribution(
  scores: number[],
): QualityDistribution {
  if (scores.length === 0) {
    return { good: 0, acceptable: 0, poor: 0, min: 0, max: 0, avg: 0, total: 0 };
  }
  let good = 0;
  let acceptable = 0;
  let poor = 0;
  let min = Infinity;
  let max = -Infinity;
  let sum = 0;
  for (const s of scores) {
    const tier = qualityTier(s);
    if (tier === 'good') good++;
    else if (tier === 'acceptable') acceptable++;
    else poor++;
    if (s < min) min = s;
    if (s > max) max = s;
    sum += s;
  }
  return {
    good,
    acceptable,
    poor,
    min: Math.round(min * 10) / 10,
    max: Math.round(max * 10) / 10,
    avg: Math.round((sum / scores.length) * 10) / 10,
    total: scores.length,
  };
}

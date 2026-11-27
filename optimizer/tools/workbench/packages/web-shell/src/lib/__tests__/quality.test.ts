// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import {
  qualityTier,
  qualityTierLabel,
  savingsPercent,
  formatSavingsPercent,
  contentClassColor,
  formatBadgeColor,
  computeQualityDistribution,
  formatToMimeType,
  findMatchingOriginal,
} from '../quality';

describe('qualityTier', () => {
  it('returns "good" for scores >= 70', () => {
    expect(qualityTier(70)).toBe('good');
    expect(qualityTier(85.5)).toBe('good');
    expect(qualityTier(100)).toBe('good');
  });

  it('returns "acceptable" for scores 50-69', () => {
    expect(qualityTier(50)).toBe('acceptable');
    expect(qualityTier(60)).toBe('acceptable');
    expect(qualityTier(69.9)).toBe('acceptable');
  });

  it('returns "poor" for scores < 50', () => {
    expect(qualityTier(0)).toBe('poor');
    expect(qualityTier(30)).toBe('poor');
    expect(qualityTier(49.9)).toBe('poor');
  });
});

describe('qualityTierLabel', () => {
  it('returns human-readable labels for each tier', () => {
    expect(qualityTierLabel('good')).toContain('near-lossless');
    expect(qualityTierLabel('acceptable')).toContain('minor artifacts');
    expect(qualityTierLabel('poor')).toContain('degradation');
  });
});

describe('savingsPercent', () => {
  it('computes savings correctly', () => {
    expect(savingsPercent(1000, 150)).toBeCloseTo(85);
    expect(savingsPercent(200, 200)).toBe(0);
    expect(savingsPercent(100, 50)).toBe(50);
  });

  it('returns 0 for zero original size', () => {
    expect(savingsPercent(0, 100)).toBe(0);
  });

  it('returns negative for size increase', () => {
    expect(savingsPercent(100, 150)).toBeLessThan(0);
  });
});

describe('formatSavingsPercent', () => {
  it('formats positive savings with minus sign', () => {
    expect(formatSavingsPercent(1000, 155)).toBe('-84.5%');
  });

  it('formats size increase with plus sign', () => {
    expect(formatSavingsPercent(100, 112)).toBe('+12.0%');
  });

  it('formats zero savings', () => {
    expect(formatSavingsPercent(100, 100)).toBe('0%');
  });

  it('handles zero original size', () => {
    expect(formatSavingsPercent(0, 100)).toBe('0%');
  });
});

describe('contentClassColor', () => {
  it('returns known colors for recognized classes', () => {
    const photo = contentClassColor('photo');
    expect(photo.bg).toContain('photo');
    expect(photo.fg).toContain('photo');
  });

  it('returns fallback for unknown classes', () => {
    const unknown = contentClassColor('unknown');
    expect(unknown.bg).toContain('tertiary');
    expect(unknown.fg).toContain('secondary');
  });
});

describe('formatBadgeColor', () => {
  it('returns colors for known formats', () => {
    expect(formatBadgeColor('webp').bg).toContain('webp');
    expect(formatBadgeColor('avif').fg).toContain('avif');
    expect(formatBadgeColor('jpeg').bg).toContain('jpeg');
    expect(formatBadgeColor('png').fg).toContain('png');
  });

  it('handles case insensitivity', () => {
    expect(formatBadgeColor('WebP').bg).toContain('webp');
    expect(formatBadgeColor('AVIF').fg).toContain('avif');
  });

  it('returns fallback for unknown formats', () => {
    const unknown = formatBadgeColor('bmp');
    expect(unknown.bg).toContain('tertiary');
  });
});

describe('computeQualityDistribution', () => {
  it('returns zeros for empty input', () => {
    const dist = computeQualityDistribution([]);
    expect(dist.total).toBe(0);
    expect(dist.good).toBe(0);
    expect(dist.acceptable).toBe(0);
    expect(dist.poor).toBe(0);
    expect(dist.min).toBe(0);
    expect(dist.max).toBe(0);
    expect(dist.avg).toBe(0);
  });

  it('computes distribution for mixed scores', () => {
    const scores = [80, 55, 30, 75, 45, 65, 90];
    const dist = computeQualityDistribution(scores);

    expect(dist.total).toBe(7);
    expect(dist.good).toBe(3); // 80, 75, 90
    expect(dist.acceptable).toBe(2); // 55, 65
    expect(dist.poor).toBe(2); // 30, 45
    expect(dist.min).toBe(30);
    expect(dist.max).toBe(90);
    expect(dist.avg).toBeCloseTo(62.9, 0);
  });

  it('handles single score', () => {
    const dist = computeQualityDistribution([72.5]);
    expect(dist.total).toBe(1);
    expect(dist.good).toBe(1);
    expect(dist.acceptable).toBe(0);
    expect(dist.poor).toBe(0);
    expect(dist.min).toBe(72.5);
    expect(dist.max).toBe(72.5);
    expect(dist.avg).toBe(72.5);
  });

  it('handles all scores in same tier', () => {
    const dist = computeQualityDistribution([80, 85, 90, 95]);
    expect(dist.good).toBe(4);
    expect(dist.acceptable).toBe(0);
    expect(dist.poor).toBe(0);
  });
});

describe('formatToMimeType', () => {
  it('maps known formats to MIME types', () => {
    expect(formatToMimeType('webp')).toBe('image/webp');
    expect(formatToMimeType('avif')).toBe('image/avif');
    expect(formatToMimeType('svg')).toBe('image/svg+xml');
  });

  it('uses origin content type for original format', () => {
    expect(formatToMimeType('original', 'image/jpeg')).toBe('image/jpeg');
    expect(formatToMimeType('original', 'image/png')).toBe('image/png');
  });

  it('returns "unknown" when no origin type for original', () => {
    expect(formatToMimeType('original')).toBe('unknown');
  });

  it('falls back to origin type for unrecognized formats', () => {
    expect(formatToMimeType('bmp', 'image/bmp')).toBe('image/bmp');
  });
});

describe('findMatchingOriginal', () => {
  const mkAlt = (format: string, viewport: string, density: string, save_data: boolean) => ({
    format,
    viewport,
    density,
    save_data,
    size: 1000,
    is_sentinel: false,
  });

  it('finds matching original by viewport/density/save_data', () => {
    const alts = [
      mkAlt('original', 'desktop', '1x', false),
      mkAlt('original', 'mobile', '1x', false),
      mkAlt('webp', 'mobile', '1x', false),
    ];
    const target = alts[2]; // webp mobile
    const match = findMatchingOriginal(target, alts);
    expect(match).toBe(alts[1]); // original mobile
  });

  it('returns undefined when no matching original exists', () => {
    const alts = [
      mkAlt('original', 'desktop', '1x', false),
      mkAlt('webp', 'mobile', '1x', false),
    ];
    const target = alts[1]; // webp mobile
    const match = findMatchingOriginal(target, alts);
    expect(match).toBeUndefined();
  });

  it('matches save_data flag', () => {
    const alts = [
      mkAlt('original', 'mobile', '1x', false),
      mkAlt('original', 'mobile', '1x', true),
      mkAlt('webp', 'mobile', '1x', true),
    ];
    const target = alts[2]; // webp mobile save_data=on
    const match = findMatchingOriginal(target, alts);
    expect(match).toBe(alts[1]); // original mobile save_data=on
  });

  it('skips sentinels', () => {
    const alts = [
      { ...mkAlt('original', 'mobile', '1x', false), is_sentinel: true },
      mkAlt('webp', 'mobile', '1x', false),
    ];
    const target = alts[1];
    const match = findMatchingOriginal(target, alts);
    expect(match).toBeUndefined();
  });

  it('matches density correctly with 1x and 2x originals', () => {
    const alts = [
      mkAlt('original', 'mobile', '1x', false),
      mkAlt('original', 'mobile', '2x', false),
      mkAlt('webp', 'mobile', '2x', false),
    ];
    const target = alts[2]; // webp mobile 2x
    const match = findMatchingOriginal(target, alts);
    expect(match).toBe(alts[1]); // original mobile 2x
  });
});

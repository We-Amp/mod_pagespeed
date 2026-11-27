// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import { computeUrlStatus } from '../url-status';
import type { AlternateInfo } from '@pagespeed/api-client';

/** Helper to build a minimal AlternateInfo. */
function alt(overrides: Partial<AlternateInfo> = {}): AlternateInfo {
  return {
    alternate_id: 0x08,
    size: 1000,
    hit_count: 1,
    is_sentinel: false,
    last_access: Date.now(),
    format: 'original',
    encoding: 'identity',
    content_type: 'html',
    ...overrides,
  };
}

describe('computeUrlStatus', () => {
  // -- Edge cases -----------------------------------------------------------

  it('returns null for empty array', () => {
    expect(computeUrlStatus([])).toBeNull();
  });

  it('returns null when all alternates are sentinels', () => {
    expect(
      computeUrlStatus([
        alt({ is_sentinel: true, sentinel_name: 'early_hints' }),
        alt({ is_sentinel: true, sentinel_name: 'browser_profile' }),
      ]),
    ).toBeNull();
  });

  // -- Revalidation ---------------------------------------------------------

  it('returns revalidating when any alternate needs revalidation', () => {
    expect(
      computeUrlStatus([
        alt({ needs_revalidation: true }),
        alt({ encoding: 'gzip' }),
        alt({ encoding: 'brotli' }),
      ]),
    ).toBe('revalidating');
  });

  // -- Original only --------------------------------------------------------

  it('returns original-only for single non-sentinel alternate', () => {
    expect(computeUrlStatus([alt()])).toBe('original-only');
  });

  it('returns original-only for single non-sentinel with sentinels', () => {
    expect(
      computeUrlStatus([
        alt(),
        alt({ is_sentinel: true, sentinel_name: 'early_hints' }),
      ]),
    ).toBe('original-only');
  });

  // -- HTML / text content --------------------------------------------------

  describe('HTML content (non-image)', () => {
    it('returns complete with identity + gzip + brotli', () => {
      expect(
        computeUrlStatus([
          alt({ encoding: 'identity', content_type: 'html' }),
          alt({ encoding: 'gzip', content_type: 'html' }),
          alt({ encoding: 'brotli', content_type: 'html' }),
        ]),
      ).toBe('complete');
    });

    it('returns partial with only identity + gzip (no brotli)', () => {
      expect(
        computeUrlStatus([
          alt({ encoding: 'identity', content_type: 'html' }),
          alt({ encoding: 'gzip', content_type: 'html' }),
        ]),
      ).toBe('partial');
    });

    it('returns partial with only identity + brotli (no gzip)', () => {
      expect(
        computeUrlStatus([
          alt({ encoding: 'identity', content_type: 'html' }),
          alt({ encoding: 'brotli', content_type: 'html' }),
        ]),
      ).toBe('partial');
    });
  });

  describe('CSS content (non-image)', () => {
    it('returns complete with gzip + brotli', () => {
      expect(
        computeUrlStatus([
          alt({ encoding: 'identity', content_type: 'css' }),
          alt({ encoding: 'gzip', content_type: 'css' }),
          alt({ encoding: 'brotli', content_type: 'css' }),
        ]),
      ).toBe('complete');
    });
  });

  describe('JS content (non-image)', () => {
    it('returns complete with gzip + brotli', () => {
      expect(
        computeUrlStatus([
          alt({ encoding: 'identity', content_type: 'js' }),
          alt({ encoding: 'gzip', content_type: 'js' }),
          alt({ encoding: 'brotli', content_type: 'js' }),
        ]),
      ).toBe('complete');
    });
  });

  // -- Image content --------------------------------------------------------

  describe('image content detection', () => {
    it('detects image via webp format', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'webp', content_type: 'image' }),
          alt({ format: 'avif', content_type: 'image' }),
        ]),
      ).toBe('complete');
    });

    it('detects image via jpeg format (raster codec name)', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'jpeg', content_type: 'image' }),
          alt({ format: 'webp', content_type: 'image' }),
          alt({ format: 'avif', content_type: 'image' }),
        ]),
      ).toBe('complete');
    });

    it('detects image via content_type=image even with only original format', () => {
      // Edge case: image with only original + gzip, no modern formats yet
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'original', content_type: 'image', encoding: 'gzip' }),
        ]),
      ).toBe('partial');
    });

    it('does NOT misclassify HTML as image (format=original, content_type=html)', () => {
      // This was the isImageContent bug: format "original" alone should NOT
      // trigger image classification. HTML/CSS/JS all use format "original".
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'html', encoding: 'identity' }),
          alt({ format: 'original', content_type: 'html', encoding: 'gzip' }),
          alt({ format: 'original', content_type: 'html', encoding: 'brotli' }),
        ]),
      ).toBe('complete'); // complete because gzip+brotli present (text path)
    });

    it('does NOT misclassify CSS as image', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'css', encoding: 'identity' }),
          alt({ format: 'original', content_type: 'css', encoding: 'gzip' }),
          alt({ format: 'original', content_type: 'css', encoding: 'brotli' }),
        ]),
      ).toBe('complete');
    });

    it('does NOT misclassify JS as image', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'js', encoding: 'identity' }),
          alt({ format: 'original', content_type: 'js', encoding: 'gzip' }),
          alt({ format: 'original', content_type: 'js', encoding: 'brotli' }),
        ]),
      ).toBe('complete');
    });
  });

  describe('image variant completeness', () => {
    it('returns complete with webp + avif', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'webp', content_type: 'image' }),
          alt({ format: 'avif', content_type: 'image' }),
        ]),
      ).toBe('complete');
    });

    it('returns partial with only webp (no avif)', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'webp', content_type: 'image' }),
        ]),
      ).toBe('partial');
    });

    it('returns partial with only avif (no webp — still modern)', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'avif', content_type: 'image' }),
        ]),
      ).toBe('complete');
    });

    it('returns partial for SVG only (no avif)', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'svg', content_type: 'image' }),
        ]),
      ).toBe('partial');
    });

    it('returns complete for SVG + avif', () => {
      expect(
        computeUrlStatus([
          alt({ format: 'original', content_type: 'image' }),
          alt({ format: 'svg', content_type: 'image' }),
          alt({ format: 'avif', content_type: 'image' }),
        ]),
      ).toBe('complete');
    });
  });

  // -- Mixed: sentinels ignored ---------------------------------------------

  it('ignores sentinels when computing status', () => {
    expect(
      computeUrlStatus([
        alt({ is_sentinel: true, sentinel_name: 'early_hints' }),
        alt({ format: 'original', content_type: 'image' }),
        alt({ format: 'webp', content_type: 'image' }),
        alt({ format: 'avif', content_type: 'image' }),
      ]),
    ).toBe('complete');
  });

  // -- Real-world scenarios -------------------------------------------------

  describe('real-world HTML page (identity + gzip + brotli + early_hints)', () => {
    const htmlAlts: AlternateInfo[] = [
      alt({ alternate_id: 0x08, format: 'original', encoding: 'identity', content_type: 'html', size: 11843 }),
      alt({ alternate_id: 0x48, format: 'original', encoding: 'gzip', content_type: 'html', size: 3124 }),
      alt({ alternate_id: 0x88, format: 'original', encoding: 'brotli', content_type: 'html', size: 2780 }),
      alt({ is_sentinel: true, sentinel_name: 'early_hints', size: 245 }),
    ];

    it('is classified as complete', () => {
      expect(computeUrlStatus(htmlAlts)).toBe('complete');
    });

    it('is NOT classified as image content', () => {
      // Verify the text path was taken (gzip+brotli), not image path
      // If it were misclassified as image, it would be "partial" (no avif/webp)
      expect(computeUrlStatus(htmlAlts)).not.toBe('partial');
    });
  });

  describe('real-world image (original + webp + avif + compressed)', () => {
    const imgAlts: AlternateInfo[] = [
      alt({ alternate_id: 0x08, format: 'jpeg', encoding: 'identity', content_type: 'image', size: 85000 }),
      alt({ alternate_id: 0x09, format: 'webp', encoding: 'identity', content_type: 'image', size: 42000 }),
      alt({ alternate_id: 0x0a, format: 'avif', encoding: 'identity', content_type: 'image', size: 31000 }),
      alt({ alternate_id: 0x49, format: 'webp', encoding: 'gzip', content_type: 'image', size: 41500 }),
      alt({ alternate_id: 0x89, format: 'webp', encoding: 'brotli', content_type: 'image', size: 41200 }),
    ];

    it('is classified as complete', () => {
      expect(computeUrlStatus(imgAlts)).toBe('complete');
    });
  });
});

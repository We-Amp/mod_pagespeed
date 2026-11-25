// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { ConfigResponse } from '@pagespeed/api-client';

// ---------------------------------------------------------------------------
// Config presets — predefined quality/feature profiles
// ---------------------------------------------------------------------------

export interface ConfigPreset {
  name: string;
  description: string;
  values: Partial<ConfigResponse>;
}

/**
 * Balanced: the worker's default settings. Good trade-off between quality
 * and file size for most sites.
 */
const BALANCED: ConfigPreset = {
  name: 'Balanced',
  description: 'Default settings — good quality/size trade-off',
  values: {
    jpeg_quality: 85,
    webp_quality: 75,
    avif_quality: 25,
    savedata_jpeg_quality: 65,
    savedata_webp_quality: 55,
    savedata_avif_quality: 35,
    target_ssimulacra2: 70,
    ssimulacra2_tolerance: 5,
    mobile_width: 480,
    tablet_width: 768,
    desktop_width: 1280,
    gzip_level: 6,
    brotli_level: 4,
    content_analysis: true,
    quality_verify: true,
    proactive_image_variants: true,
    proactive_viewport_variants: false,
    proactive_savedata_variants: false,
    proactive_density_variants: false,
    cache_mode: 'safe',
    html_mode: 'full',
    disable_css: false,
    disable_js: false,
    disable_image: false,
    disable_lazy_load: false,
    disable_image_dimensions: false,
    disable_lcp_preload: false,
    disable_preconnect_injection: false,
    enable_speculation_rules: false,
    disable_async_css: false,
    disable_css_import_flattening: false,
    disable_script_deferral: false,
    learned_quality: true,
    learned_quality_jpeg: true,
    learned_quality_webp: true,
    learned_quality_avif: true,
  },
};

const AGGRESSIVE: ConfigPreset = {
  name: 'Aggressive Compression',
  description: 'Lower quality, smaller files — bandwidth priority',
  values: {
    jpeg_quality: 65,
    webp_quality: 55,
    avif_quality: 35,
    savedata_jpeg_quality: 50,
    savedata_webp_quality: 40,
    savedata_avif_quality: 40,
    target_ssimulacra2: 55,
    ssimulacra2_tolerance: 8,
    gzip_level: 9,
    brotli_level: 6,
    content_analysis: true,
    quality_verify: true,
    proactive_image_variants: true,
    proactive_viewport_variants: true,
    proactive_savedata_variants: true,
    proactive_density_variants: true,
    cache_mode: 'aggressive',
    disable_async_css: false,
    disable_script_deferral: false,
    learned_quality: true,
    learned_quality_jpeg: true,
    learned_quality_webp: true,
    learned_quality_avif: true,
  },
};

const MAX_QUALITY: ConfigPreset = {
  name: 'Maximum Quality',
  description: 'Higher quality, larger files — visual fidelity priority',
  values: {
    jpeg_quality: 95,
    webp_quality: 90,
    avif_quality: 15,
    savedata_jpeg_quality: 80,
    savedata_webp_quality: 70,
    savedata_avif_quality: 25,
    target_ssimulacra2: 85,
    ssimulacra2_tolerance: 3,
    gzip_level: 4,
    brotli_level: 3,
    content_analysis: true,
    quality_verify: true,
    proactive_image_variants: true,
    proactive_viewport_variants: false,
    proactive_savedata_variants: false,
    proactive_density_variants: false,
    cache_mode: 'safe',
    disable_async_css: false,
    disable_script_deferral: false,
    learned_quality: true,
    learned_quality_jpeg: true,
    learned_quality_webp: true,
    learned_quality_avif: true,
  },
};

const BANDWIDTH_SAVER: ConfigPreset = {
  name: 'Bandwidth Saver',
  description: 'Save-Data optimized — aggressive compression everywhere',
  values: {
    jpeg_quality: 50,
    webp_quality: 40,
    avif_quality: 40,
    savedata_jpeg_quality: 40,
    savedata_webp_quality: 30,
    savedata_avif_quality: 45,
    target_ssimulacra2: 45,
    ssimulacra2_tolerance: 10,
    gzip_level: 9,
    brotli_level: 6,
    content_analysis: true,
    quality_verify: false,
    proactive_image_variants: true,
    proactive_viewport_variants: true,
    proactive_savedata_variants: true,
    proactive_density_variants: true,
    cache_mode: 'aggressive',
    disable_async_css: false,
    disable_script_deferral: false,
    learned_quality: true,
    learned_quality_jpeg: true,
    learned_quality_webp: true,
    learned_quality_avif: true,
  },
};

/** All available presets, in display order. */
export const PRESETS: ConfigPreset[] = [
  BALANCED,
  AGGRESSIVE,
  MAX_QUALITY,
  BANDWIDTH_SAVER,
];

/** Default values used by the Reset action. */
export const DEFAULT_VALUES = BALANCED.values;

// ---------------------------------------------------------------------------
// Config field metadata — drives the UI layout
// ---------------------------------------------------------------------------

export interface QualityFieldMeta {
  key: keyof ConfigResponse;
  label: string;
  min: number;
  max: number;
  step: number;
  /** Quality value below which an orange warning appears. */
  warningBelow?: number;
  /** Quality value above which an orange warning appears. */
  warningAbove?: number;
  /** Help text shown via HelpIcon tooltip. */
  tooltip?: string;
}

export interface ToggleFieldMeta {
  key: keyof ConfigResponse;
  label: string;
  description: string;
  /** If true, the field semantics are "disable_X" — UI shows inverted. */
  invertDisplay?: boolean;
}

export interface NumberFieldMeta {
  key: keyof ConfigResponse;
  label: string;
  min: number;
  max: number;
  step: number;
  unit?: string;
  /** Help text shown via HelpIcon tooltip. */
  tooltip?: string;
}

// -- Quality sliders --

export const IMAGE_QUALITY_FIELDS: QualityFieldMeta[] = [
  { key: 'jpeg_quality', label: 'JPEG Quality', min: 1, max: 100, step: 1, warningBelow: 30, tooltip: 'JPEG compression quality (1-100). Higher = better quality, larger files.' },
  { key: 'webp_quality', label: 'WebP Quality', min: 1, max: 100, step: 1, warningBelow: 30, tooltip: 'WebP compression quality (1-100). Typically achieves same visual quality as JPEG at lower values.' },
  { key: 'avif_quality', label: 'AVIF Quality', min: 1, max: 63, step: 1, warningBelow: 10, tooltip: 'AVIF compression quality (1-63). Lower = better quality (inverted from JPEG/WebP). Very efficient codec.' },
];

export const SAVEDATA_QUALITY_FIELDS: QualityFieldMeta[] = [
  { key: 'savedata_jpeg_quality', label: 'Save-Data JPEG', min: 1, max: 100, step: 1, warningBelow: 30, tooltip: 'JPEG quality when Save-Data header is present. Lower to save bandwidth for data-constrained users.' },
  { key: 'savedata_webp_quality', label: 'Save-Data WebP', min: 1, max: 100, step: 1, warningBelow: 30, tooltip: 'WebP quality for Save-Data mode. Applied when the browser sends Save-Data: on.' },
  { key: 'savedata_avif_quality', label: 'Save-Data AVIF', min: 1, max: 63, step: 1, warningBelow: 10, tooltip: 'AVIF quality for Save-Data mode (1-63). Lower values = better quality, larger files.' },
];

export const SSIMULACRA2_FIELDS: QualityFieldMeta[] = [
  { key: 'target_ssimulacra2', label: 'SSIMULACRA2 Target', min: 0, max: 100, step: 1, warningAbove: 90, tooltip: 'Target perceptual quality score (0-100). 80+ = imperceptible, 70-80 = very good, 60-70 = good. Worker re-encodes if output falls below target.' },
  { key: 'ssimulacra2_tolerance', label: 'SSIMULACRA2 Tolerance', min: 0, max: 20, step: 1, tooltip: 'Tolerance range for quality verification. Avoids costly re-encoding for small deviations from the target score.' },
];

export const QUALITY_CAP_FIELDS: NumberFieldMeta[] = [
  { key: 'quality_cap_margin', label: 'Quality Cap Margin', min: 0, max: 50, step: 1, tooltip: 'Margin in quality points above the original. Prevents re-encoding at higher quality than the source (wastes bytes without improving visuals).' },
  { key: 'savedata_score_reduction', label: 'Save-Data Score Reduction', min: 0, max: 50, step: 1, tooltip: 'SSIMULACRA2 points to subtract from the target score when Save-Data header is present. Higher = more aggressive compression for data-constrained users.' },
];

export const QUALITY_CAP_TOGGLES: ToggleFieldMeta[] = [
  { key: 'no_quality_cap', label: 'Disable Quality Cap', description: 'Allow re-encoding at higher quality than the original (may increase file size)' },
];

// -- Feature toggles --

export const IMAGE_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'disable_image', label: 'Image Optimization', description: 'Optimize images (WebP, AVIF transcoding)', invertDisplay: true },
  { key: 'content_analysis', label: 'Content Analysis', description: 'Classify images (photo, screenshot, illustration)' },
  { key: 'quality_verify', label: 'Quality Verification', description: 'Verify output quality with SSIMULACRA2' },
  { key: 'proactive_image_variants', label: 'Proactive Image Variants', description: 'Pre-generate WebP/AVIF variants' },
  { key: 'proactive_viewport_variants', label: 'Proactive Viewport Variants', description: 'Pre-generate viewport-specific variants' },
  { key: 'proactive_density_variants', label: 'Proactive Density Variants', description: 'Pre-generate 1x/2x density variants' },
  { key: 'proactive_savedata_variants', label: 'Proactive Save-Data Variants', description: 'Pre-generate Save-Data variants' },
];

export const LEARNED_QUALITY_TOGGLES: ToggleFieldMeta[] = [
  { key: 'learned_quality', label: 'Learned Quality Prediction', description: 'Use ML models to predict optimal encoder quality' },
  { key: 'learned_quality_jpeg', label: 'Learned Quality (JPEG)', description: 'ML quality prediction for JPEG encoding' },
  { key: 'learned_quality_webp', label: 'Learned Quality (WebP)', description: 'ML quality prediction for WebP encoding' },
  { key: 'learned_quality_avif', label: 'Learned Quality (AVIF)', description: 'ML quality prediction for AVIF encoding' },
];

export const HTML_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'disable_lazy_load', label: 'Lazy Loading', description: 'Add loading="lazy" to offscreen images', invertDisplay: true },
  { key: 'disable_image_dimensions', label: 'Image Dimensions', description: 'Add width/height to prevent CLS', invertDisplay: true },
  { key: 'disable_lcp_preload', label: 'LCP Preload', description: 'Inject <link rel=preload> for LCP image', invertDisplay: true },
  { key: 'disable_preconnect_injection', label: 'Preconnect Injection', description: 'Inject <link rel=preconnect> for third-party origins', invertDisplay: true },
  { key: 'enable_speculation_rules', label: 'Speculation Rules', description: 'Inject speculation rules for prefetching' },
  { key: 'disable_async_css', label: 'Async CSS Loading', description: 'Load CSS asynchronously to prevent render blocking', invertDisplay: true },
];

export const CSS_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'disable_css', label: 'CSS Processing', description: 'Process and optimize CSS responses', invertDisplay: true },
  { key: 'disable_css_import_flattening', label: 'CSS Import Flattening', description: 'Flatten @import rules into the main stylesheet', invertDisplay: true },
];

export const JS_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'disable_js', label: 'JS Processing', description: 'Process JavaScript responses', invertDisplay: true },
  { key: 'disable_script_deferral', label: 'Script Deferral', description: 'Defer non-critical scripts to improve page load', invertDisplay: true },
];

export const GENERAL_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'enable_warmup', label: 'Hot URL Warmup', description: 'Pre-generate variants for frequently accessed URLs' },
];

export const BROWSER_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'enable_browser_analysis', label: 'Browser Analysis', description: 'Enable Chrome-based page analysis for critical CSS, LCP, and lazy loading' },
  { key: 'enable_browser_critical_css', label: 'Critical CSS', description: 'Extract critical CSS using Chrome CSS Coverage API' },
  { key: 'enable_browser_lazy_loading', label: 'Lazy Loading Analysis', description: 'Browser-validated lazy loading decisions' },
  { key: 'enable_browser_lcp_preload', label: 'LCP Preload', description: 'Browser-detected LCP candidate preloading' },
  { key: 'enable_browser_image_sizing', label: 'Image Sizing', description: 'Browser-measured image dimensions for CLS prevention' },
];

export const BROWSER_FIELDS: NumberFieldMeta[] = [
  { key: 'chrome_recycle_interval', label: 'Chrome Recycle Interval', min: 1, max: 10000, step: 1, tooltip: 'Pages before Chrome process is recycled to prevent memory leaks. Default: 100.' },
  { key: 'chrome_page_timeout', label: 'Page Timeout', min: 1000, max: 300000, step: 1000, unit: 'ms', tooltip: 'Maximum time for Chrome to load and analyze a page. Default: 60000ms.' },
  { key: 'chrome_max_memory', label: 'Chrome Max Memory', min: 128, max: 4096, step: 64, unit: 'MB', tooltip: 'Maximum RSS memory before Chrome process is killed and restarted. Default: 512MB.' },
  { key: 'browser_queue_size', label: 'Queue Size', min: 1, max: 100000, step: 1, tooltip: 'Maximum pending analysis jobs. Head-drops oldest when full. Default: 1000.' },
  { key: 'browser_profile_ttl', label: 'Profile TTL', min: 60, max: 604800, step: 60, unit: 's', tooltip: 'How long browser profiles are cached before expiry. Default: 86400s (24h).' },
];

// -- Advanced numeric fields --

export const COMPRESSION_FIELDS: NumberFieldMeta[] = [
  { key: 'gzip_level', label: 'Gzip Level', min: 1, max: 9, step: 1, tooltip: 'Gzip compression level (1-9). Higher = better compression, slower. Default: 6.' },
  { key: 'brotli_level', label: 'Brotli Level', min: 1, max: 11, step: 1, tooltip: 'Brotli compression level (1-11). Higher = better compression, slower. Default: 4.' },
];

export const VIEWPORT_FIELDS: NumberFieldMeta[] = [
  { key: 'mobile_width', label: 'Mobile Width', min: 240, max: 1024, step: 1, unit: 'px', tooltip: 'Maximum viewport width classified as mobile. Images are resized to this width for mobile variants.' },
  { key: 'tablet_width', label: 'Tablet Width', min: 480, max: 2048, step: 1, unit: 'px', tooltip: 'Maximum viewport width classified as tablet. Between mobile and desktop breakpoints.' },
  { key: 'desktop_width', label: 'Desktop Width', min: 768, max: 3840, step: 1, unit: 'px', tooltip: 'Target width for desktop viewport class. Images resized to this width for desktop variants.' },
];

export const SVG_FIELDS: NumberFieldMeta[] = [
  { key: 'svg_fidelity_threshold', label: 'SVG Fidelity Threshold', min: 0, max: 100, step: 1, tooltip: 'Minimum fidelity score (0-100) for accepting an SVG vectorization result. Higher = stricter quality gate, fewer SVGs produced.' },
  { key: 'svg_timeout_ms', label: 'SVG Timeout', min: 100, max: 30000, step: 100, unit: 'ms', tooltip: 'Maximum time in milliseconds for the VTracer vectorization pass. Increase for complex images; decrease to skip slow candidates.' },
  { key: 'svg_candidacy_threshold', label: 'SVG Candidacy Threshold', min: 0, max: 100, step: 1, tooltip: 'Content analysis score threshold for SVG candidacy evaluation. Lower = more images considered for vectorization.' },
  { key: 'svg_color_precision', label: 'SVG Color Precision', min: 0, max: 8, step: 1, tooltip: 'Color quantization precision for VTracer. 0 = automatic. Higher values preserve more color detail in the SVG output.' },
  { key: 'svg_filter_speckle', label: 'SVG Filter Speckle', min: 0, max: 100, step: 1, tooltip: 'Minimum cluster size in pixels. Smaller clusters are filtered out as noise. Higher = cleaner SVG with fewer small details.' },
  { key: 'svg_max_paths', label: 'SVG Max Paths', min: 10, max: 10000, step: 10, tooltip: 'Maximum number of SVG paths before rejecting the result. Limits file size for overly complex vectorizations.' },
  { key: 'svg_max_pixels', label: 'SVG Max Pixels', min: 1024, max: 1048576, step: 1024, tooltip: 'Maximum total pixels (width x height) for SVG candidacy. Larger images are skipped to avoid slow vectorization.' },
  { key: 'svg_max_svg_bytes', label: 'SVG Max Size', min: 1024, max: 10485760, step: 1024, unit: 'bytes', tooltip: 'Maximum SVG output size in bytes. Vectorized SVGs larger than this are rejected (original raster served instead).' },
];

export interface SelectFieldMeta {
  key: keyof ConfigResponse;
  label: string;
  options: { value: string; label: string }[];
  /** Help text shown via HelpIcon tooltip. */
  tooltip?: string;
}

export const SVG_FEATURE_TOGGLES: ToggleFieldMeta[] = [
  { key: 'svg_exclude_lcp', label: 'Exclude LCP Images', description: 'Skip SVG vectorization for LCP images (prevents slower initial paint from SVG decoding)' },
];

export const SVG_PRESET_FIELD: SelectFieldMeta = {
  key: 'svg_preset',
  label: 'SVG Preset',
  options: [
    { value: 'default', label: 'Default' },
    { value: 'detailed', label: 'Detailed' },
    { value: 'simple', label: 'Simple' },
  ],
  tooltip: 'VTracer preset controlling vectorization detail level. "Simple" produces smaller SVGs; "Detailed" preserves more visual fidelity.',
};

export const CACHE_MODE_SELECT: SelectFieldMeta = {
  key: 'cache_mode',
  label: 'Cache Mode',
  options: [
    { value: 'safe', label: 'Safe — short TTLs, must-revalidate' },
    { value: 'aggressive', label: 'Aggressive — long TTLs, public' },
  ],
  tooltip: 'Safe mode uses short cache TTLs with must-revalidate for quick recovery from misconfigurations. Aggressive mode uses long TTLs for maximum cache efficiency.',
};

export const HTML_MODE_SELECT: SelectFieldMeta = {
  key: 'html_mode',
  label: 'HTML Mode',
  options: [
    { value: 'full', label: 'Full — optimize HTML' },
    { value: 'markdown-static', label: 'Markdown only — static' },
    { value: 'markdown-rendered', label: 'Markdown only — rendered' },
    { value: 'off', label: 'Off — no HTML processing' },
  ],
  tooltip: 'Full optimizes HTML responses (and builds the Markdown copy for agents when agent optimization is on). The Markdown-only modes write no optimized HTML. Off disables HTML processing; it replaces the old HTML Processing switch.',
};

export const SVG_MODE_FIELD: SelectFieldMeta = {
  key: 'svg_mode',
  label: 'SVG Mode',
  options: [
    { value: 'detect', label: 'Detect' },
    { value: 'always', label: 'Always' },
    { value: 'never', label: 'Never' },
  ],
  tooltip: 'SVG vectorization mode. "Detect" uses content analysis to select candidates. "Always" vectorizes all eligible images. "Never" disables SVG vectorization.',
};

export const LIMIT_FIELDS: NumberFieldMeta[] = [
  { key: 'max_url_length', label: 'Max URL Length', min: 256, max: 65536, step: 1, unit: 'chars', tooltip: 'Maximum URL length to process (64-65536 chars). Longer URLs are passed through unchanged.' },
  { key: 'max_html_size', label: 'Max HTML Size', min: 1024, max: 104857600, step: 1024, unit: 'bytes', tooltip: 'Maximum HTML document size to process (1KB-64MB). Larger documents are passed through unchanged.' },
  { key: 'max_css_size', label: 'Max CSS Size', min: 1024, max: 104857600, step: 1024, unit: 'bytes', tooltip: 'Maximum CSS file size to process (1KB-64MB). Larger stylesheets are passed through unchanged.' },
  { key: 'max_js_size', label: 'Max JS Size', min: 1024, max: 104857600, step: 1024, unit: 'bytes', tooltip: 'Maximum JavaScript file size to process (1KB-64MB). Larger scripts are passed through unchanged.' },
  { key: 'max_image_size', label: 'Max Image Size', min: 1024, max: 104857600, step: 1024, unit: 'bytes', tooltip: 'Maximum image file size to process (1KB-256MB). Larger images are passed through unchanged.' },
];

// ---------------------------------------------------------------------------
// Utility: compute changed fields between two config objects
// ---------------------------------------------------------------------------

/**
 * Returns only the fields in `current` that differ from `baseline`.
 * Useful for PATCH requests that should only send changed fields.
 */
export function changedFields(
  current: ConfigResponse,
  baseline: ConfigResponse,
): Partial<ConfigResponse> {
  const patch: Partial<ConfigResponse> = {};
  for (const key of Object.keys(current) as Array<keyof ConfigResponse>) {
    if (current[key] !== baseline[key]) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      (patch as any)[key] = current[key];
    }
  }
  return patch;
}

/**
 * Count the number of fields that differ between two configs.
 */
export function changedFieldCount(
  current: ConfigResponse,
  baseline: ConfigResponse,
): number {
  return Object.keys(changedFields(current, baseline)).length;
}

/**
 * Apply a preset's values onto a config, returning a new config object.
 */
export function applyPreset(
  config: ConfigResponse,
  preset: ConfigPreset,
): ConfigResponse {
  return { ...config, ...preset.values };
}

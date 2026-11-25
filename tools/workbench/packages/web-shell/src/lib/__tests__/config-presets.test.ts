// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import type { ConfigResponse } from '@pagespeed/api-client';
import {
  PRESETS,
  DEFAULT_VALUES,
  changedFields,
  changedFieldCount,
  applyPreset,
  IMAGE_QUALITY_FIELDS,
  SAVEDATA_QUALITY_FIELDS,
  SSIMULACRA2_FIELDS,
  IMAGE_FEATURE_TOGGLES,
  LEARNED_QUALITY_TOGGLES,
  HTML_FEATURE_TOGGLES,
  HTML_MODE_SELECT,
  CSS_FEATURE_TOGGLES,
  JS_FEATURE_TOGGLES,
  COMPRESSION_FIELDS,
  VIEWPORT_FIELDS,
  SVG_FIELDS,
  SVG_FEATURE_TOGGLES,
  SVG_PRESET_FIELD,
  SVG_MODE_FIELD,
  QUALITY_CAP_FIELDS,
  QUALITY_CAP_TOGGLES,
  GENERAL_FEATURE_TOGGLES,
  BROWSER_FEATURE_TOGGLES,
  BROWSER_FIELDS,
} from '../config-presets';
import type { ConfigPreset } from '../config-presets';

/** Build a full ConfigResponse with optional overrides. */
function makeConfig(
  overrides: Partial<ConfigResponse> = {},
): ConfigResponse {
  return {
    jpeg_quality: 85,
    webp_quality: 75,
    avif_quality: 25,
    savedata_jpeg_quality: 65,
    savedata_webp_quality: 55,
    savedata_avif_quality: 35,
    mobile_width: 480,
    tablet_width: 768,
    desktop_width: 1280,
    proactive_image_variants: true,
    proactive_viewport_variants: false,
    proactive_savedata_variants: false,
    proactive_density_variants: false,
    content_analysis: true,
    quality_verify: true,
    target_ssimulacra2: 70,
    ssimulacra2_tolerance: 5,
    html_mode: 'full',
    disable_html: false,
    disable_css: false,
    disable_js: false,
    disable_script_deferral: false,
    disable_image: false,
    disable_lazy_load: false,
    disable_image_dimensions: false,
    disable_lcp_preload: false,
    disable_preconnect_injection: false,
    enable_speculation_rules: false,
    disable_async_css: false,
    disable_css_import_flattening: false,
    svg_preset: 'default',
    svg_fidelity_threshold: 70,
    svg_timeout_ms: 5000,
    svg_candidacy_threshold: 50,
    svg_color_precision: 0,
    svg_exclude_lcp: true,
    svg_filter_speckle: 4,
    svg_max_paths: 500,
    svg_max_pixels: 65536,
    svg_max_svg_bytes: 262144,
    svg_mode: 'detect',
    no_quality_cap: false,
    quality_cap_margin: 10,
    savedata_score_reduction: 15,
    gzip_level: 6,
    brotli_level: 4,
    max_url_length: 8192,
    max_html_size: 10485760,
    max_css_size: 10485760,
    max_js_size: 10485760,
    max_image_size: 10485760,
    enable_browser_analysis: false,
    chrome_binary: '/usr/bin/chrome-headless-shell',
    chrome_recycle_interval: 100,
    chrome_page_timeout: 60000,
    chrome_max_memory: 512,
    enable_browser_critical_css: true,
    enable_browser_lazy_loading: true,
    enable_browser_lcp_preload: true,
    enable_browser_image_sizing: true,
    browser_queue_size: 1000,
    browser_profile_ttl: 86400,
    learned_quality: true,
    learned_quality_jpeg: true,
    learned_quality_webp: true,
    learned_quality_avif: true,
    enable_warmup: false,
    cache_mode: 'safe',
    ...overrides,
  };
}

describe('PRESETS', () => {
  it('contains 4 presets', () => {
    expect(PRESETS).toHaveLength(4);
  });

  it('has unique names', () => {
    const names = PRESETS.map((p) => p.name);
    expect(new Set(names).size).toBe(names.length);
  });

  it('each preset has a name, description, and values', () => {
    for (const preset of PRESETS) {
      expect(preset.name).toBeTruthy();
      expect(preset.description).toBeTruthy();
      expect(Object.keys(preset.values).length).toBeGreaterThan(0);
    }
  });

  it('Balanced preset has jpeg_quality of 85', () => {
    const balanced = PRESETS.find((p) => p.name === 'Balanced');
    expect(balanced).toBeDefined();
    expect(balanced!.values.jpeg_quality).toBe(85);
  });

  it('Aggressive preset has lower quality values', () => {
    const aggressive = PRESETS.find(
      (p) => p.name === 'Aggressive Compression',
    );
    expect(aggressive).toBeDefined();
    expect(aggressive!.values.jpeg_quality).toBeLessThan(85);
    expect(aggressive!.values.webp_quality).toBeLessThan(75);
  });

  it('Maximum Quality preset has higher quality values', () => {
    const maxQ = PRESETS.find((p) => p.name === 'Maximum Quality');
    expect(maxQ).toBeDefined();
    expect(maxQ!.values.jpeg_quality).toBeGreaterThan(85);
    expect(maxQ!.values.webp_quality).toBeGreaterThan(75);
  });

  it('Bandwidth Saver preset has lowest quality values', () => {
    const bw = PRESETS.find((p) => p.name === 'Bandwidth Saver');
    expect(bw).toBeDefined();
    expect(bw!.values.jpeg_quality).toBeLessThan(65);
  });

  it('all presets include async CSS and script deferral flags', () => {
    for (const preset of PRESETS) {
      expect(
        preset.values.disable_async_css,
        `${preset.name} should have disable_async_css`,
      ).toBeDefined();
      expect(
        preset.values.disable_script_deferral,
        `${preset.name} should have disable_script_deferral`,
      ).toBeDefined();
    }
  });

  it('all presets include learned_quality flags', () => {
    for (const preset of PRESETS) {
      expect(
        preset.values.learned_quality,
        `${preset.name} should have learned_quality`,
      ).toBeDefined();
      expect(
        preset.values.learned_quality_jpeg,
        `${preset.name} should have learned_quality_jpeg`,
      ).toBeDefined();
      expect(
        preset.values.learned_quality_webp,
        `${preset.name} should have learned_quality_webp`,
      ).toBeDefined();
      expect(
        preset.values.learned_quality_avif,
        `${preset.name} should have learned_quality_avif`,
      ).toBeDefined();
    }
  });
});

describe('DEFAULT_VALUES', () => {
  it('matches the Balanced preset values', () => {
    const balanced = PRESETS.find((p) => p.name === 'Balanced');
    expect(DEFAULT_VALUES).toEqual(balanced!.values);
  });
});

describe('changedFields', () => {
  it('returns empty object when configs are identical', () => {
    const a = makeConfig();
    const b = makeConfig();
    expect(changedFields(a, b)).toEqual({});
  });

  it('detects changed numeric field', () => {
    const a = makeConfig({ jpeg_quality: 90 });
    const b = makeConfig({ jpeg_quality: 85 });
    const patch = changedFields(a, b);
    expect(patch).toEqual({ jpeg_quality: 90 });
  });

  it('detects changed boolean field', () => {
    const a = makeConfig({ disable_html: true });
    const b = makeConfig({ disable_html: false });
    const patch = changedFields(a, b);
    expect(patch).toEqual({ disable_html: true });
  });

  it('detects multiple changed fields', () => {
    const a = makeConfig({ jpeg_quality: 50, webp_quality: 40 });
    const b = makeConfig({ jpeg_quality: 85, webp_quality: 75 });
    const patch = changedFields(a, b);
    expect(patch).toEqual({ jpeg_quality: 50, webp_quality: 40 });
  });

  it('ignores unchanged fields', () => {
    const a = makeConfig({ jpeg_quality: 50 });
    const b = makeConfig({ jpeg_quality: 85 });
    const patch = changedFields(a, b);
    expect(Object.keys(patch)).toEqual(['jpeg_quality']);
  });
});

describe('changedFieldCount', () => {
  it('returns 0 for identical configs', () => {
    const cfg = makeConfig();
    expect(changedFieldCount(cfg, cfg)).toBe(0);
  });

  it('returns correct count for changed fields', () => {
    const a = makeConfig({ jpeg_quality: 50, webp_quality: 40 });
    const b = makeConfig();
    expect(changedFieldCount(a, b)).toBe(2);
  });
});

describe('applyPreset', () => {
  it('applies preset values onto existing config', () => {
    const cfg = makeConfig();
    const preset: ConfigPreset = {
      name: 'Test',
      description: 'Test preset',
      values: { jpeg_quality: 42, webp_quality: 33 },
    };
    const result = applyPreset(cfg, preset);
    expect(result.jpeg_quality).toBe(42);
    expect(result.webp_quality).toBe(33);
    // Unchanged fields should remain.
    expect(result.avif_quality).toBe(cfg.avif_quality);
    expect(result.mobile_width).toBe(cfg.mobile_width);
  });

  it('does not mutate the original config', () => {
    const cfg = makeConfig();
    const original_jpeg = cfg.jpeg_quality;
    const preset: ConfigPreset = {
      name: 'Test',
      description: 'Test',
      values: { jpeg_quality: 10 },
    };
    applyPreset(cfg, preset);
    expect(cfg.jpeg_quality).toBe(original_jpeg);
  });
});

describe('field metadata', () => {
  it('IMAGE_QUALITY_FIELDS has 3 fields', () => {
    expect(IMAGE_QUALITY_FIELDS).toHaveLength(3);
  });

  it('SAVEDATA_QUALITY_FIELDS has 3 fields', () => {
    expect(SAVEDATA_QUALITY_FIELDS).toHaveLength(3);
  });

  it('SSIMULACRA2_FIELDS has 2 fields', () => {
    expect(SSIMULACRA2_FIELDS).toHaveLength(2);
  });

  it('SSIMULACRA2 target has warningAbove of 90', () => {
    const target = SSIMULACRA2_FIELDS.find(
      (f) => f.key === 'target_ssimulacra2',
    );
    expect(target).toBeDefined();
    expect(target!.warningAbove).toBe(90);
  });

  it('all quality fields have valid min < max', () => {
    const all = [
      ...IMAGE_QUALITY_FIELDS,
      ...SAVEDATA_QUALITY_FIELDS,
      ...SSIMULACRA2_FIELDS,
    ];
    for (const field of all) {
      expect(field.min).toBeLessThan(field.max);
    }
  });

  it('all toggle fields have unique keys', () => {
    const allToggles = [
      ...IMAGE_FEATURE_TOGGLES,
      ...LEARNED_QUALITY_TOGGLES,
      ...HTML_FEATURE_TOGGLES,
      ...CSS_FEATURE_TOGGLES,
      ...JS_FEATURE_TOGGLES,
      ...GENERAL_FEATURE_TOGGLES,
      ...BROWSER_FEATURE_TOGGLES,
      ...SVG_FEATURE_TOGGLES,
      ...QUALITY_CAP_TOGGLES,
    ];
    const keys = allToggles.map((t) => t.key);
    expect(new Set(keys).size).toBe(keys.length);
  });

  it('IMAGE_FEATURE_TOGGLES has 7 toggles', () => {
    expect(IMAGE_FEATURE_TOGGLES).toHaveLength(7);
  });

  it('LEARNED_QUALITY_TOGGLES has 4 toggles', () => {
    expect(LEARNED_QUALITY_TOGGLES).toHaveLength(4);
  });

  it('HTML_FEATURE_TOGGLES has 6 toggles', () => {
    expect(HTML_FEATURE_TOGGLES).toHaveLength(6);
  });

  it('HTML processing is the html_mode select, not a disable_html toggle', () => {
    expect(HTML_FEATURE_TOGGLES.map((t) => t.key)).not.toContain('disable_html');
    expect(HTML_MODE_SELECT.key).toBe('html_mode');
    expect(HTML_MODE_SELECT.options.map((o) => o.value)).toEqual([
      'full',
      'markdown-static',
      'markdown-rendered',
      'off',
    ]);
  });

  it('CSS_FEATURE_TOGGLES has 2 toggles', () => {
    expect(CSS_FEATURE_TOGGLES).toHaveLength(2);
  });

  it('JS_FEATURE_TOGGLES has 2 toggles', () => {
    expect(JS_FEATURE_TOGGLES).toHaveLength(2);
  });

  it('GENERAL_FEATURE_TOGGLES has 1 toggle', () => {
    expect(GENERAL_FEATURE_TOGGLES).toHaveLength(1);
  });

  it('BROWSER_FEATURE_TOGGLES has 5 toggles', () => {
    expect(BROWSER_FEATURE_TOGGLES).toHaveLength(5);
  });

  it('BROWSER_FIELDS has 5 fields', () => {
    expect(BROWSER_FIELDS).toHaveLength(5);
  });

  it('BROWSER_FIELDS have valid min < max', () => {
    for (const field of BROWSER_FIELDS) {
      expect(field.min).toBeLessThan(field.max);
    }
  });

  it('BROWSER_FIELDS have tooltip text', () => {
    for (const field of BROWSER_FIELDS) {
      expect(
        field.tooltip,
        `${field.key} should have tooltip text`,
      ).toBeTruthy();
      expect(
        field.tooltip!.length,
        `${field.key} tooltip should be descriptive (>20 chars)`,
      ).toBeGreaterThan(20);
    }
  });

  it('COMPRESSION_FIELDS has 2 fields', () => {
    expect(COMPRESSION_FIELDS).toHaveLength(2);
  });

  it('VIEWPORT_FIELDS has 3 fields', () => {
    expect(VIEWPORT_FIELDS).toHaveLength(3);
  });

  it('SVG_FIELDS has 8 fields', () => {
    expect(SVG_FIELDS).toHaveLength(8);
  });

  it('SVG_PRESET_FIELD has 3 options', () => {
    expect(SVG_PRESET_FIELD.options).toHaveLength(3);
    expect(SVG_PRESET_FIELD.key).toBe('svg_preset');
  });

  it('SVG_FIELDS have valid min < max', () => {
    for (const field of SVG_FIELDS) {
      expect(field.min).toBeLessThan(field.max);
    }
  });

  it('SVG_FIELDS have tooltip text', () => {
    for (const field of SVG_FIELDS) {
      expect(
        field.tooltip,
        `${field.key} should have tooltip text`,
      ).toBeTruthy();
      expect(
        field.tooltip!.length,
        `${field.key} tooltip should be descriptive (>20 chars)`,
      ).toBeGreaterThan(20);
    }
  });

  it('SVG_FEATURE_TOGGLES has 1 toggle', () => {
    expect(SVG_FEATURE_TOGGLES).toHaveLength(1);
    expect(SVG_FEATURE_TOGGLES[0].key).toBe('svg_exclude_lcp');
  });

  it('SVG_MODE_FIELD has 3 options', () => {
    expect(SVG_MODE_FIELD.options).toHaveLength(3);
    expect(SVG_MODE_FIELD.key).toBe('svg_mode');
  });

  it('QUALITY_CAP_FIELDS has 2 fields', () => {
    expect(QUALITY_CAP_FIELDS).toHaveLength(2);
  });

  it('QUALITY_CAP_TOGGLES has 1 toggle', () => {
    expect(QUALITY_CAP_TOGGLES).toHaveLength(1);
    expect(QUALITY_CAP_TOGGLES[0].key).toBe('no_quality_cap');
  });

  it('SVG_PRESET_FIELD has tooltip text', () => {
    expect(SVG_PRESET_FIELD.tooltip).toBeTruthy();
    expect(SVG_PRESET_FIELD.tooltip!.length).toBeGreaterThan(20);
  });

  it('all quality slider fields have tooltip text', () => {
    const allSliderFields = [
      ...IMAGE_QUALITY_FIELDS,
      ...SAVEDATA_QUALITY_FIELDS,
      ...SSIMULACRA2_FIELDS,
    ];
    for (const field of allSliderFields) {
      expect(
        field.tooltip,
        `${field.key} should have tooltip text`,
      ).toBeTruthy();
      expect(
        field.tooltip!.length,
        `${field.key} tooltip should be descriptive (>20 chars)`,
      ).toBeGreaterThan(20);
    }
  });

  it('all number fields have tooltip text', () => {
    const allNumberFields = [
      ...COMPRESSION_FIELDS,
      ...VIEWPORT_FIELDS,
      ...SVG_FIELDS,
      ...QUALITY_CAP_FIELDS,
    ];
    for (const field of allNumberFields) {
      expect(
        field.tooltip,
        `${field.key} should have tooltip text`,
      ).toBeTruthy();
      expect(
        field.tooltip!.length,
        `${field.key} tooltip should be descriptive (>20 chars)`,
      ).toBeGreaterThan(20);
    }
  });

  it('no duplicate tooltip text across fields', () => {
    const allTooltips = [
      ...IMAGE_QUALITY_FIELDS.map((f) => f.tooltip),
      ...SAVEDATA_QUALITY_FIELDS.map((f) => f.tooltip),
      ...SSIMULACRA2_FIELDS.map((f) => f.tooltip),
      ...COMPRESSION_FIELDS.map((f) => f.tooltip),
      ...VIEWPORT_FIELDS.map((f) => f.tooltip),
      ...SVG_FIELDS.map((f) => f.tooltip),
      ...BROWSER_FIELDS.map((f) => f.tooltip),
      ...QUALITY_CAP_FIELDS.map((f) => f.tooltip),
    ].filter(Boolean);
    expect(new Set(allTooltips).size).toBe(allTooltips.length);
  });

  it('all field keys correspond to ConfigResponse properties', () => {
    // Build a set of known ConfigResponse keys from a dummy config.
    const cfg = makeConfig();
    const validKeys = new Set(Object.keys(cfg));

    const allFields = [
      ...IMAGE_QUALITY_FIELDS,
      ...SAVEDATA_QUALITY_FIELDS,
      ...SSIMULACRA2_FIELDS,
      ...IMAGE_FEATURE_TOGGLES,
      ...LEARNED_QUALITY_TOGGLES,
      ...HTML_FEATURE_TOGGLES,
      ...CSS_FEATURE_TOGGLES,
      ...JS_FEATURE_TOGGLES,
      ...GENERAL_FEATURE_TOGGLES,
      ...BROWSER_FEATURE_TOGGLES,
      ...SVG_FEATURE_TOGGLES,
      ...QUALITY_CAP_TOGGLES,
      ...COMPRESSION_FIELDS,
      ...VIEWPORT_FIELDS,
      ...SVG_FIELDS,
      ...QUALITY_CAP_FIELDS,
      ...BROWSER_FIELDS,
      SVG_MODE_FIELD,
      SVG_PRESET_FIELD,
    ];
    for (const field of allFields) {
      expect(
        validKeys.has(field.key),
        `${field.key} should be a valid ConfigResponse key`,
      ).toBe(true);
    }
  });
});

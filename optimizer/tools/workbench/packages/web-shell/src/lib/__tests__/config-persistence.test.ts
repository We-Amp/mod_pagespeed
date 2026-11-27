// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// @vitest-environment jsdom
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import type { ConfigResponse } from '@pagespeed/api-client';
import {
  exportConfigFile,
  generateCliFlags,
  validateImport,
  createSnapshot,
  loadSnapshots,
  saveSnapshots,
  addSnapshot,
  deleteSnapshot,
  diffSnapshots,
} from '../config-persistence';
import type { ConfigExportFile, ConfigSnapshot } from '../config-persistence';

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
    disable_async_css: false,
    disable_image: false,
    disable_lazy_load: false,
    disable_image_dimensions: false,
    disable_lcp_preload: false,
    disable_preconnect_injection: false,
    enable_speculation_rules: false,
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
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// generateCliFlags
// ---------------------------------------------------------------------------

describe('generateCliFlags', () => {
  it('returns empty string when config matches defaults', () => {
    const cfg = makeConfig();
    const flags = generateCliFlags(cfg, cfg);
    expect(flags).toBe('');
  });

  it('generates flags for changed numeric fields', () => {
    const defaults = makeConfig();
    const cfg = makeConfig({ jpeg_quality: 50, webp_quality: 40 });
    const flags = generateCliFlags(cfg, defaults);
    expect(flags).toContain('--jpeg_quality=50');
    expect(flags).toContain('--webp_quality=40');
  });

  it('generates flags for changed boolean fields', () => {
    const defaults = makeConfig();
    const cfg = makeConfig({ disable_html: true });
    const flags = generateCliFlags(cfg, defaults);
    expect(flags).toContain('--disable_html=true');
  });

  it('formats boolean values as true/false strings', () => {
    const defaults = makeConfig({ content_analysis: false });
    const cfg = makeConfig({ content_analysis: true });
    const flags = generateCliFlags(cfg, defaults);
    expect(flags).toContain('--content_analysis=true');
  });

  it('separates flags with backslash-newline-indent', () => {
    const defaults = makeConfig();
    const cfg = makeConfig({ jpeg_quality: 50, webp_quality: 40 });
    const flags = generateCliFlags(cfg, defaults);
    expect(flags).toContain(' \\\n  ');
  });
});

// ---------------------------------------------------------------------------
// validateImport
// ---------------------------------------------------------------------------

describe('validateImport', () => {
  it('accepts a valid export file', () => {
    const cfg = makeConfig();
    const data: ConfigExportFile = {
      format: 'pagespeed-config',
      version: 1,
      exported_at: '2024-01-15T10:30:00Z',
      description: 'Test export',
      config: cfg,
    };
    const result = validateImport(data);
    expect(result.valid).toBe(true);
    expect(result.config).toEqual(cfg);
    expect(result.description).toBe('Test export');
    expect(result.errors).toEqual([]);
  });

  it('rejects non-object input', () => {
    const result = validateImport('not an object');
    expect(result.valid).toBe(false);
    expect(result.errors.length).toBeGreaterThan(0);
  });

  it('rejects null input', () => {
    const result = validateImport(null);
    expect(result.valid).toBe(false);
  });

  it('rejects wrong format field', () => {
    const result = validateImport({
      format: 'wrong-format',
      version: 1,
      config: makeConfig(),
    });
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes('format'))).toBe(true);
  });

  it('rejects missing version', () => {
    const result = validateImport({
      format: 'pagespeed-config',
      config: makeConfig(),
    });
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes('version'))).toBe(true);
  });

  it('rejects missing config object', () => {
    const result = validateImport({
      format: 'pagespeed-config',
      version: 1,
    });
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes('config'))).toBe(true);
  });

  it('rejects config with wrong types', () => {
    const result = validateImport({
      format: 'pagespeed-config',
      version: 1,
      config: {
        jpeg_quality: 'not a number',
        disable_html: 42,
      },
    });
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes('jpeg_quality'))).toBe(
      true,
    );
    expect(result.errors.some((e) => e.includes('disable_html'))).toBe(
      true,
    );
  });

  it('accepts config with extra unknown fields', () => {
    const cfg = makeConfig();
    const data = {
      format: 'pagespeed-config',
      version: 1,
      exported_at: '2024-01-15T10:30:00Z',
      description: '',
      config: { ...cfg, unknown_field: 'hello' },
    };
    const result = validateImport(data);
    expect(result.valid).toBe(true);
  });

  it('accepts config with missing optional fields', () => {
    const data = {
      format: 'pagespeed-config',
      version: 1,
      exported_at: '2024-01-15T10:30:00Z',
      description: '',
      config: { jpeg_quality: 85 },
    };
    const result = validateImport(data);
    expect(result.valid).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// exportConfigFile (browser download mock)
// ---------------------------------------------------------------------------

describe('exportConfigFile', () => {
  let createObjectURLMock: ReturnType<typeof vi.fn>;
  let clickSpy: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    createObjectURLMock = vi.fn(() => 'blob:mock-url');
    clickSpy = vi.fn();

    globalThis.URL.createObjectURL = createObjectURLMock;
    globalThis.URL.revokeObjectURL = vi.fn();

    const origCreateElement = document.createElement.bind(document);
    vi.spyOn(document, 'createElement').mockImplementation((tag: string) => {
      if (tag === 'a') {
        const el = origCreateElement('a');
        el.click = clickSpy;
        el.remove = vi.fn();
        return el;
      }
      return origCreateElement(tag);
    });

    vi.spyOn(document.body, 'appendChild').mockImplementation(
      (node) => node as HTMLElement,
    );
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('creates a blob and triggers download', () => {
    const cfg = makeConfig();
    exportConfigFile(cfg, 'Test description');
    expect(createObjectURLMock).toHaveBeenCalledTimes(1);
    expect(clickSpy).toHaveBeenCalledTimes(1);
  });

  it('includes format, version, description, and config in the blob', async () => {
    const cfg = makeConfig({ jpeg_quality: 42 });
    exportConfigFile(cfg, 'My description');

    const blob = createObjectURLMock.mock.calls[0][0] as Blob;
    const text = await new Promise<string>((resolve) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result as string);
      reader.readAsText(blob);
    });
    const parsed = JSON.parse(text);
    expect(parsed.format).toBe('pagespeed-config');
    expect(parsed.version).toBe(1);
    expect(parsed.description).toBe('My description');
    expect(parsed.config.jpeg_quality).toBe(42);
  });
});

// ---------------------------------------------------------------------------
// Snapshots (localStorage)
// ---------------------------------------------------------------------------

describe('config snapshots', () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it('loadSnapshots returns empty array when nothing saved', () => {
    expect(loadSnapshots()).toEqual([]);
  });

  it('saveSnapshots and loadSnapshots roundtrip', () => {
    const snap: ConfigSnapshot = {
      id: 'test-1',
      name: 'Test',
      created_at: '2024-01-15T10:30:00Z',
      config: makeConfig(),
    };
    saveSnapshots([snap]);
    const loaded = loadSnapshots();
    expect(loaded).toHaveLength(1);
    expect(loaded[0].id).toBe('test-1');
    expect(loaded[0].name).toBe('Test');
  });

  it('createSnapshot generates unique ID', () => {
    const a = createSnapshot('A', makeConfig());
    const b = createSnapshot('B', makeConfig());
    expect(a.id).not.toBe(b.id);
    expect(a.name).toBe('A');
    expect(b.name).toBe('B');
  });

  it('addSnapshot persists and returns updated list', () => {
    const cfg = makeConfig();
    const result = addSnapshot('First', cfg);
    expect(result).toHaveLength(1);
    expect(result[0].name).toBe('First');

    const result2 = addSnapshot('Second', cfg);
    expect(result2).toHaveLength(2);
    // Newest first.
    expect(result2[0].name).toBe('Second');
    expect(result2[1].name).toBe('First');
  });

  it('deleteSnapshot removes by ID', () => {
    const cfg = makeConfig();
    addSnapshot('First', cfg);
    const snaps = addSnapshot('Second', cfg);
    const idToDelete = snaps[0].id;

    const result = deleteSnapshot(idToDelete);
    expect(result).toHaveLength(1);
    expect(result[0].name).toBe('First');
  });

  it('loadSnapshots handles corrupted localStorage gracefully', () => {
    localStorage.setItem('pagespeed-config-snapshots', 'not json');
    expect(loadSnapshots()).toEqual([]);
  });

  it('loadSnapshots handles non-array JSON gracefully', () => {
    localStorage.setItem(
      'pagespeed-config-snapshots',
      JSON.stringify({ foo: 'bar' }),
    );
    expect(loadSnapshots()).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// diffSnapshots
// ---------------------------------------------------------------------------

describe('diffSnapshots', () => {
  it('returns empty array for identical configs', () => {
    const cfg = makeConfig();
    const diff = diffSnapshots(cfg, cfg);
    expect(diff).toEqual([]);
  });

  it('returns changed fields', () => {
    const a = makeConfig({ jpeg_quality: 85 });
    const b = makeConfig({ jpeg_quality: 50 });
    const diff = diffSnapshots(a, b);
    expect(diff).toHaveLength(1);
    expect(diff[0].key).toBe('jpeg_quality');
    expect(diff[0].valueA).toBe(85);
    expect(diff[0].valueB).toBe(50);
  });

  it('formats field labels from snake_case', () => {
    const a = makeConfig({ jpeg_quality: 85 });
    const b = makeConfig({ jpeg_quality: 50 });
    const diff = diffSnapshots(a, b);
    expect(diff[0].label).toBe('Jpeg Quality');
  });

  it('detects multiple differences', () => {
    const a = makeConfig({ jpeg_quality: 85, disable_html: false });
    const b = makeConfig({ jpeg_quality: 50, disable_html: true });
    const diff = diffSnapshots(a, b);
    expect(diff.length).toBeGreaterThanOrEqual(2);
    const keys = diff.map((d) => d.key);
    expect(keys).toContain('jpeg_quality');
    expect(keys).toContain('disable_html');
  });
});

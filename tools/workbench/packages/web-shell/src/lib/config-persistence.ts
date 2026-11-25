// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// Config export/import and snapshot management for the config panel.
// ---------------------------------------------------------------------------

import type { ConfigResponse } from '@pagespeed/api-client';
import { exportJson, timestampedFilename } from './export';
import { changedFields } from './config-presets';

// ---------------------------------------------------------------------------
// Export file format
// ---------------------------------------------------------------------------

/** The shape of a `.pagespeed.json` export file. */
export interface ConfigExportFile {
  /** File format identifier. */
  format: 'pagespeed-config';
  /** Export format version (for forward-compat). */
  version: 1;
  /** ISO-8601 timestamp when the file was exported. */
  exported_at: string;
  /** Optional user-provided description. */
  description: string;
  /** The full config state at the time of export. */
  config: ConfigResponse;
}

// ---------------------------------------------------------------------------
// Export functionality
// ---------------------------------------------------------------------------

/**
 * Export the current config as a `.pagespeed.json` file download.
 */
export function exportConfigFile(
  config: ConfigResponse,
  description: string = '',
): void {
  const payload: ConfigExportFile = {
    format: 'pagespeed-config',
    version: 1,
    exported_at: new Date().toISOString(),
    description,
    config,
  };
  const filename = timestampedFilename('pagespeed-config', 'pagespeed.json');
  exportJson(payload, filename);
}

/**
 * Generate a CLI flags string from config values that differ from defaults.
 * Each field becomes `--field_name=value`.
 */
export function generateCliFlags(
  config: ConfigResponse,
  defaults: Partial<ConfigResponse>,
): string {
  const fullDefaults = defaults as ConfigResponse;
  const patch = changedFields(config, fullDefaults);
  const flags: string[] = [];
  for (const [key, value] of Object.entries(patch)) {
    if (typeof value === 'boolean') {
      flags.push(`--${key}=${value ? 'true' : 'false'}`);
    } else {
      flags.push(`--${key}=${value}`);
    }
  }
  return flags.join(' \\\n  ');
}

/**
 * Copy text to the clipboard. SSR-safe.
 * Returns true on success, false on failure or SSR.
 */
export async function copyToClipboard(text: string): Promise<boolean> {
  if (typeof navigator === 'undefined' || !navigator.clipboard) return false;
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Import functionality
// ---------------------------------------------------------------------------

/** Result of validating an imported config file. */
export interface ImportValidationResult {
  valid: boolean;
  config: ConfigResponse | null;
  description: string;
  exported_at: string;
  errors: string[];
}

/** Known keys and their expected types for ConfigResponse. */
const CONFIG_KEY_TYPES: Record<string, 'number' | 'boolean' | 'string'> = {
  jpeg_quality: 'number',
  webp_quality: 'number',
  avif_quality: 'number',
  savedata_jpeg_quality: 'number',
  savedata_webp_quality: 'number',
  savedata_avif_quality: 'number',
  mobile_width: 'number',
  tablet_width: 'number',
  desktop_width: 'number',
  proactive_image_variants: 'boolean',
  proactive_viewport_variants: 'boolean',
  proactive_savedata_variants: 'boolean',
  proactive_density_variants: 'boolean',
  content_analysis: 'boolean',
  quality_verify: 'boolean',
  target_ssimulacra2: 'number',
  ssimulacra2_tolerance: 'number',
  html_mode: 'string',
  disable_html: 'boolean',
  disable_css: 'boolean',
  disable_js: 'boolean',
  disable_image: 'boolean',
  disable_lazy_load: 'boolean',
  disable_image_dimensions: 'boolean',
  disable_lcp_preload: 'boolean',
  disable_preconnect_injection: 'boolean',
  enable_speculation_rules: 'boolean',
  disable_css_import_flattening: 'boolean',
  gzip_level: 'number',
  brotli_level: 'number',
  max_url_length: 'number',
  max_html_size: 'number',
  max_css_size: 'number',
  max_js_size: 'number',
  max_image_size: 'number',
  svg_preset: 'string',
  svg_fidelity_threshold: 'number',
  svg_timeout_ms: 'number',
  enable_browser_analysis: 'boolean',
  chrome_binary: 'string',
  chrome_recycle_interval: 'number',
  chrome_page_timeout: 'number',
  chrome_max_memory: 'number',
  enable_browser_critical_css: 'boolean',
  enable_browser_lazy_loading: 'boolean',
  enable_browser_lcp_preload: 'boolean',
  enable_browser_image_sizing: 'boolean',
  browser_queue_size: 'number',
  browser_profile_ttl: 'number',
  enable_warmup: 'boolean',
};

/**
 * Validate parsed JSON against the ConfigExportFile schema.
 * Returns the config if valid, or a list of errors.
 */
export function validateImport(data: unknown): ImportValidationResult {
  const errors: string[] = [];

  if (typeof data !== 'object' || data === null) {
    return {
      valid: false,
      config: null,
      description: '',
      exported_at: '',
      errors: ['File does not contain a valid JSON object.'],
    };
  }

  const obj = data as Record<string, unknown>;

  if (obj.format !== 'pagespeed-config') {
    errors.push(
      `Invalid format: expected "pagespeed-config", got "${String(obj.format)}".`,
    );
  }

  if (typeof obj.version !== 'number' || obj.version < 1) {
    errors.push(`Invalid or missing version field.`);
  }

  const description =
    typeof obj.description === 'string' ? obj.description : '';
  const exported_at =
    typeof obj.exported_at === 'string' ? obj.exported_at : '';

  if (typeof obj.config !== 'object' || obj.config === null) {
    errors.push('Missing or invalid "config" object.');
    return { valid: false, config: null, description, exported_at, errors };
  }

  const cfg = obj.config as Record<string, unknown>;

  // Validate each known key's type.
  for (const [key, expectedType] of Object.entries(CONFIG_KEY_TYPES)) {
    if (key in cfg) {
      const actualType = typeof cfg[key];
      if (actualType !== expectedType) {
        errors.push(
          `config.${key}: expected ${expectedType}, got ${actualType}.`,
        );
      }
    }
    // Missing keys are acceptable — they won't be patched.
  }

  if (errors.length > 0) {
    return { valid: false, config: null, description, exported_at, errors };
  }

  return {
    valid: true,
    config: cfg as unknown as ConfigResponse,
    description,
    exported_at,
    errors: [],
  };
}

/**
 * Read and validate a `.pagespeed.json` file from a File input.
 */
export async function readConfigFile(
  file: File,
): Promise<ImportValidationResult> {
  try {
    const text = await file.text();
    const data = JSON.parse(text);
    return validateImport(data);
  } catch (err) {
    return {
      valid: false,
      config: null,
      description: '',
      exported_at: '',
      errors: [
        `Failed to parse file: ${err instanceof Error ? err.message : String(err)}`,
      ],
    };
  }
}

// ---------------------------------------------------------------------------
// Config Snapshots (localStorage)
// ---------------------------------------------------------------------------

export interface ConfigSnapshot {
  /** Unique identifier (timestamp-based). */
  id: string;
  /** User-provided name. */
  name: string;
  /** ISO-8601 timestamp. */
  created_at: string;
  /** The full config at snapshot time. */
  config: ConfigResponse;
}

const SNAPSHOTS_STORAGE_KEY = 'pagespeed-config-snapshots';

/**
 * Load all saved snapshots from localStorage.
 */
export function loadSnapshots(): ConfigSnapshot[] {
  if (typeof window === 'undefined') return [];
  try {
    const raw = localStorage.getItem(SNAPSHOTS_STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed as ConfigSnapshot[];
  } catch {
    return [];
  }
}

/**
 * Save all snapshots to localStorage.
 */
export function saveSnapshots(snapshots: ConfigSnapshot[]): void {
  if (typeof window === 'undefined') return;
  localStorage.setItem(SNAPSHOTS_STORAGE_KEY, JSON.stringify(snapshots));
}

/**
 * Create a new snapshot from the current config.
 */
export function createSnapshot(
  name: string,
  config: ConfigResponse,
): ConfigSnapshot {
  return {
    id: `snap-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
    name,
    created_at: new Date().toISOString(),
    config: { ...config },
  };
}

/**
 * Add a new snapshot and persist.
 */
export function addSnapshot(
  name: string,
  config: ConfigResponse,
): ConfigSnapshot[] {
  const snapshots = loadSnapshots();
  const snap = createSnapshot(name, config);
  snapshots.unshift(snap);
  saveSnapshots(snapshots);
  return snapshots;
}

/**
 * Delete a snapshot by ID and persist.
 */
export function deleteSnapshot(id: string): ConfigSnapshot[] {
  const snapshots = loadSnapshots().filter((s) => s.id !== id);
  saveSnapshots(snapshots);
  return snapshots;
}

// ---------------------------------------------------------------------------
// Snapshot comparison / diff
// ---------------------------------------------------------------------------

export interface ConfigDiffEntry {
  key: keyof ConfigResponse;
  label: string;
  valueA: unknown;
  valueB: unknown;
}

/**
 * Compute a diff between two config snapshots.
 * Returns only the fields that differ.
 */
export function diffSnapshots(
  a: ConfigResponse,
  b: ConfigResponse,
): ConfigDiffEntry[] {
  const entries: ConfigDiffEntry[] = [];
  for (const key of Object.keys(a) as Array<keyof ConfigResponse>) {
    if (a[key] !== b[key]) {
      entries.push({
        key,
        label: formatFieldLabel(key),
        valueA: a[key],
        valueB: b[key],
      });
    }
  }
  return entries;
}

/**
 * Convert a snake_case field key to a human-readable label.
 */
function formatFieldLabel(key: string): string {
  return key
    .replace(/_/g, ' ')
    .replace(/\b\w/g, (c) => c.toUpperCase());
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// Capability mask encoding/decoding and device presets for the mod_pagespeed
// 32-bit capability bitmask.
//
// Bit layout (low 8 bits used):
//   Bits 0-1: Image Format
//   Bits 2-3: Viewport Class
//   Bit 4:    Pixel Density
//   Bit 5:    Save-Data
//   Bits 6-7: Transfer Encoding
// ---------------------------------------------------------------------------

export enum ImageFormat {
  Original = 0,
  WebP = 1,
  AVIF = 2,
  SVG = 3,
}

export enum ViewportClass {
  Mobile = 0,
  Tablet = 1,
  Desktop = 2,
}

export enum PixelDensity {
  Standard = 0,
  High = 1,
}

export enum SaveData {
  Off = 0,
  On = 1,
}

export enum TransferEncoding {
  Identity = 0,
  Gzip = 1,
  Brotli = 2,
  Reserved = 3,
}

export interface MaskComponents {
  format: ImageFormat;
  viewport: ViewportClass;
  density: PixelDensity;
  saveData: SaveData;
  encoding: TransferEncoding;
}

/**
 * Encode mask components into an 8-bit capability mask value.
 */
export function encodeMask(c: MaskComponents): number {
  return (
    ((c.format & 0x03) << 0) |
    ((c.viewport & 0x03) << 2) |
    ((c.density & 0x01) << 4) |
    ((c.saveData & 0x01) << 5) |
    ((c.encoding & 0x03) << 6)
  );
}

/**
 * Decode an 8-bit capability mask value into its components.
 */
export function decodeMask(mask: number): MaskComponents {
  return {
    format: ((mask >> 0) & 0x03) as ImageFormat,
    viewport: ((mask >> 2) & 0x03) as ViewportClass,
    density: ((mask >> 4) & 0x01) as PixelDensity,
    saveData: ((mask >> 5) & 0x01) as SaveData,
    encoding: ((mask >> 6) & 0x03) as TransferEncoding,
  };
}

// ---------------------------------------------------------------------------
// Device presets
// ---------------------------------------------------------------------------

export interface DevicePreset {
  name: string;
  description: string;
  components: MaskComponents;
}

export const DEVICE_PRESETS: DevicePreset[] = [
  {
    name: 'iPhone 15 Pro',
    description: 'Mobile, 2x, AVIF, no save-data, Identity',
    components: {
      format: ImageFormat.AVIF,
      viewport: ViewportClass.Mobile,
      density: PixelDensity.High,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    },
  },
  {
    name: 'Pixel 8',
    description: 'Mobile, 2x, WebP, no save-data, Identity',
    components: {
      format: ImageFormat.WebP,
      viewport: ViewportClass.Mobile,
      density: PixelDensity.High,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    },
  },
  {
    name: 'iPad Air',
    description: 'Tablet, 2x, WebP, no save-data, Identity',
    components: {
      format: ImageFormat.WebP,
      viewport: ViewportClass.Tablet,
      density: PixelDensity.High,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    },
  },
  {
    name: 'Desktop Chrome',
    description: 'Desktop, 1x, AVIF, no save-data, Identity',
    components: {
      format: ImageFormat.AVIF,
      viewport: ViewportClass.Desktop,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    },
  },
  {
    name: 'Desktop Firefox',
    description: 'Desktop, 1x, WebP, no save-data, Identity',
    components: {
      format: ImageFormat.WebP,
      viewport: ViewportClass.Desktop,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    },
  },
  {
    name: 'Low Bandwidth',
    description: 'Mobile, 1x, WebP, save-data on, Identity',
    components: {
      format: ImageFormat.WebP,
      viewport: ViewportClass.Mobile,
      density: PixelDensity.Standard,
      saveData: SaveData.On,
      encoding: TransferEncoding.Identity,
    },
  },
  {
    name: 'Original Content',
    description:
      'Desktop, 1x, Original, no save-data, Identity (default mask 0x08)',
    components: {
      format: ImageFormat.Original,
      viewport: ViewportClass.Desktop,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    },
  },
];

// ---------------------------------------------------------------------------
// Human-readable formatting
// ---------------------------------------------------------------------------

const IMAGE_FORMAT_NAMES: Record<ImageFormat, string> = {
  [ImageFormat.Original]: 'Original',
  [ImageFormat.WebP]: 'WebP',
  [ImageFormat.AVIF]: 'AVIF',
  [ImageFormat.SVG]: 'SVG',
};

const VIEWPORT_NAMES: Record<ViewportClass, string> = {
  [ViewportClass.Mobile]: 'Mobile',
  [ViewportClass.Tablet]: 'Tablet',
  [ViewportClass.Desktop]: 'Desktop',
};

const DENSITY_NAMES: Record<PixelDensity, string> = {
  [PixelDensity.Standard]: '1x',
  [PixelDensity.High]: '2x+',
};

const SAVE_DATA_NAMES: Record<SaveData, string> = {
  [SaveData.Off]: 'Off',
  [SaveData.On]: 'On',
};

const ENCODING_NAMES: Record<TransferEncoding, string> = {
  [TransferEncoding.Identity]: 'Identity',
  [TransferEncoding.Gzip]: 'Gzip',
  [TransferEncoding.Brotli]: 'Brotli',
  [TransferEncoding.Reserved]: 'Reserved',
};

export function formatImageFormat(f: ImageFormat): string {
  return IMAGE_FORMAT_NAMES[f] ?? 'Unknown';
}

export function formatViewport(v: ViewportClass): string {
  return VIEWPORT_NAMES[v] ?? 'Unknown';
}

export function formatDensity(d: PixelDensity): string {
  return DENSITY_NAMES[d] ?? 'Unknown';
}

export function formatSaveData(s: SaveData): string {
  return SAVE_DATA_NAMES[s] ?? 'Unknown';
}

export function formatEncoding(e: TransferEncoding): string {
  return ENCODING_NAMES[e] ?? 'Unknown';
}

/**
 * Format a capability mask as a human-readable string.
 * Example: "Desktop / AVIF / 1x / Identity"
 */
export function formatMask(mask: number): string {
  const c = decodeMask(mask);
  return [
    formatViewport(c.viewport),
    formatImageFormat(c.format),
    formatDensity(c.density),
    formatEncoding(c.encoding),
  ].join(' / ');
}

/**
 * Format a byte count as a human-readable string.
 * Examples: "0 B", "512 B", "1.5 KB", "3.2 MB", "1.0 GB"
 */
export function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  // Find the appropriate unit index.
  const exp = Math.min(
    Math.floor(Math.log(Math.abs(bytes)) / Math.log(1024)),
    units.length - 1,
  );
  const value = bytes / Math.pow(1024, exp);
  // Use integer display for bytes, one decimal for larger units.
  if (exp === 0) return `${bytes} B`;
  return `${value.toFixed(1)} ${units[exp]}`;
}

/**
 * Format a duration in seconds as a human-readable string.
 * Examples: "0s", "45s", "3m 15s", "2h 3m 15s"
 */
export function formatUptime(seconds: number): string {
  if (seconds <= 0) return '0s';
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  const parts: string[] = [];
  if (h > 0) parts.push(`${h}h`);
  if (m > 0) parts.push(`${m}m`);
  if (s > 0 || parts.length === 0) parts.push(`${s}s`);
  return parts.join(' ');
}

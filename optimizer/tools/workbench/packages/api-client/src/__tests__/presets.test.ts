// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, it, expect } from 'vitest';
import {
  ImageFormat,
  ViewportClass,
  PixelDensity,
  SaveData,
  TransferEncoding,
  encodeMask,
  decodeMask,
  DEVICE_PRESETS,
  formatImageFormat,
  formatViewport,
  formatDensity,
  formatSaveData,
  formatEncoding,
  formatMask,
  formatBytes,
} from '../presets.js';
import type { MaskComponents } from '../presets.js';

// ---------------------------------------------------------------------------
// encodeMask / decodeMask
// ---------------------------------------------------------------------------

describe('encodeMask', () => {
  it('Original Content (default mask) encodes to 0x08', () => {
    const mask = encodeMask({
      format: ImageFormat.Original,
      viewport: ViewportClass.Desktop,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    });
    expect(mask).toBe(0x08);
  });

  it('Mobile WebP 2x encodes to 0x11', () => {
    // bits 0-1=01(WebP), bits 2-3=00(Mobile), bit 4=1(2x),
    // bit 5=0, bits 6-7=00 => 0b00_0_1_00_01 = 0x11
    const mask = encodeMask({
      format: ImageFormat.WebP,
      viewport: ViewportClass.Mobile,
      density: PixelDensity.High,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    });
    expect(mask).toBe(0x11);
  });

  it('Desktop AVIF Identity encodes to 0x0A', () => {
    // bits 0-1=10(AVIF), bits 2-3=10(Desktop), bit 4=0, bit 5=0,
    // bits 6-7=00 => 0b00_0_0_10_10 = 0x0A
    const mask = encodeMask({
      format: ImageFormat.AVIF,
      viewport: ViewportClass.Desktop,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    });
    expect(mask).toBe(0x0a);
  });

  it('Desktop Brotli encodes encoding in bits 6-7', () => {
    // bits 0-1=00(Original), bits 2-3=10(Desktop), bit 4=0, bit 5=0,
    // bits 6-7=10(Brotli) => 0b10_0_0_10_00 = 0x88
    const mask = encodeMask({
      format: ImageFormat.Original,
      viewport: ViewportClass.Desktop,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Brotli,
    });
    expect(mask).toBe(0x88);
  });

  it('Mobile save-data WebP encodes to 0x21', () => {
    // bits 0-1=01(WebP), bits 2-3=00(Mobile), bit 4=0(1x), bit 5=1(SaveData),
    // bits 6-7=00 => 0b00_1_0_00_01 = 0x21
    const mask = encodeMask({
      format: ImageFormat.WebP,
      viewport: ViewportClass.Mobile,
      density: PixelDensity.Standard,
      saveData: SaveData.On,
      encoding: TransferEncoding.Identity,
    });
    expect(mask).toBe(0x21);
  });

  it('all bits set (SVG, Reserved viewport 3, High, SaveData, Reserved encoding)', () => {
    // bits 0-1=11(SVG), bits 2-3=11(3), bit 4=1, bit 5=1,
    // bits 6-7=11(Reserved) => 0b11_1_1_11_11 = 0xFF
    const mask = encodeMask({
      format: ImageFormat.SVG,
      viewport: 3 as ViewportClass,
      density: PixelDensity.High,
      saveData: SaveData.On,
      encoding: TransferEncoding.Reserved,
    });
    expect(mask).toBe(0xff);
  });

  it('zero mask (Mobile, Original, Standard, Off, Identity) encodes to 0x00', () => {
    const mask = encodeMask({
      format: ImageFormat.Original,
      viewport: ViewportClass.Mobile,
      density: PixelDensity.Standard,
      saveData: SaveData.Off,
      encoding: TransferEncoding.Identity,
    });
    expect(mask).toBe(0x00);
  });
});

describe('decodeMask', () => {
  it('0x08 decodes to Original/Desktop/Standard/Off/Identity', () => {
    const c = decodeMask(0x08);
    expect(c.format).toBe(ImageFormat.Original);
    expect(c.viewport).toBe(ViewportClass.Desktop);
    expect(c.density).toBe(PixelDensity.Standard);
    expect(c.saveData).toBe(SaveData.Off);
    expect(c.encoding).toBe(TransferEncoding.Identity);
  });

  it('0x00 decodes to Original/Mobile/Standard/Off/Identity', () => {
    const c = decodeMask(0x00);
    expect(c.format).toBe(ImageFormat.Original);
    expect(c.viewport).toBe(ViewportClass.Mobile);
    expect(c.density).toBe(PixelDensity.Standard);
    expect(c.saveData).toBe(SaveData.Off);
    expect(c.encoding).toBe(TransferEncoding.Identity);
  });

  it('0x11 decodes to WebP/Mobile/High/Off/Identity', () => {
    const c = decodeMask(0x11);
    expect(c.format).toBe(ImageFormat.WebP);
    expect(c.viewport).toBe(ViewportClass.Mobile);
    expect(c.density).toBe(PixelDensity.High);
    expect(c.saveData).toBe(SaveData.Off);
    expect(c.encoding).toBe(TransferEncoding.Identity);
  });

  it('0x0A decodes to AVIF/Desktop/Standard/Off/Identity', () => {
    const c = decodeMask(0x0a);
    expect(c.format).toBe(ImageFormat.AVIF);
    expect(c.viewport).toBe(ViewportClass.Desktop);
    expect(c.density).toBe(PixelDensity.Standard);
    expect(c.saveData).toBe(SaveData.Off);
    expect(c.encoding).toBe(TransferEncoding.Identity);
  });

  it('0x88 decodes to Original/Desktop/Standard/Off/Brotli', () => {
    const c = decodeMask(0x88);
    expect(c.format).toBe(ImageFormat.Original);
    expect(c.viewport).toBe(ViewportClass.Desktop);
    expect(c.density).toBe(PixelDensity.Standard);
    expect(c.saveData).toBe(SaveData.Off);
    expect(c.encoding).toBe(TransferEncoding.Brotli);
  });

  it('0xFF decodes to SVG/viewport-3/High/On/Reserved', () => {
    const c = decodeMask(0xff);
    expect(c.format).toBe(ImageFormat.SVG);
    expect(c.viewport).toBe(3); // not a named ViewportClass
    expect(c.density).toBe(PixelDensity.High);
    expect(c.saveData).toBe(SaveData.On);
    expect(c.encoding).toBe(TransferEncoding.Reserved);
  });
});

describe('encodeMask/decodeMask roundtrip', () => {
  it('roundtrips all device presets', () => {
    for (const preset of DEVICE_PRESETS) {
      const mask = encodeMask(preset.components);
      const decoded = decodeMask(mask);
      expect(decoded).toEqual(preset.components);
    }
  });

  it('roundtrips all 256 possible 8-bit values', () => {
    for (let i = 0; i < 256; i++) {
      const components = decodeMask(i);
      const reencoded = encodeMask(components);
      expect(reencoded).toBe(i);
    }
  });
});

// ---------------------------------------------------------------------------
// DEVICE_PRESETS
// ---------------------------------------------------------------------------

describe('DEVICE_PRESETS', () => {
  it('contains 7 presets', () => {
    expect(DEVICE_PRESETS).toHaveLength(7);
  });

  it('iPhone 15 Pro has expected components', () => {
    const preset = DEVICE_PRESETS.find((p) => p.name === 'iPhone 15 Pro');
    expect(preset).toBeDefined();
    expect(preset!.components.format).toBe(ImageFormat.AVIF);
    expect(preset!.components.viewport).toBe(ViewportClass.Mobile);
    expect(preset!.components.density).toBe(PixelDensity.High);
    expect(preset!.components.saveData).toBe(SaveData.Off);
    expect(preset!.components.encoding).toBe(TransferEncoding.Identity);
  });

  it('Original Content preset encodes to 0x08', () => {
    const preset = DEVICE_PRESETS.find((p) => p.name === 'Original Content');
    expect(preset).toBeDefined();
    expect(encodeMask(preset!.components)).toBe(0x08);
  });

  it('Low Bandwidth preset has save-data on', () => {
    const preset = DEVICE_PRESETS.find((p) => p.name === 'Low Bandwidth');
    expect(preset).toBeDefined();
    expect(preset!.components.saveData).toBe(SaveData.On);
    expect(preset!.components.density).toBe(PixelDensity.Standard);
  });

  it('all presets have unique names', () => {
    const names = DEVICE_PRESETS.map((p) => p.name);
    expect(new Set(names).size).toBe(names.length);
  });
});

// ---------------------------------------------------------------------------
// Format functions
// ---------------------------------------------------------------------------

describe('formatImageFormat', () => {
  it('returns expected strings', () => {
    expect(formatImageFormat(ImageFormat.Original)).toBe('Original');
    expect(formatImageFormat(ImageFormat.WebP)).toBe('WebP');
    expect(formatImageFormat(ImageFormat.AVIF)).toBe('AVIF');
    expect(formatImageFormat(ImageFormat.SVG)).toBe('SVG');
  });
});

describe('formatViewport', () => {
  it('returns expected strings', () => {
    expect(formatViewport(ViewportClass.Mobile)).toBe('Mobile');
    expect(formatViewport(ViewportClass.Tablet)).toBe('Tablet');
    expect(formatViewport(ViewportClass.Desktop)).toBe('Desktop');
  });
});

describe('formatDensity', () => {
  it('returns expected strings', () => {
    expect(formatDensity(PixelDensity.Standard)).toBe('1x');
    expect(formatDensity(PixelDensity.High)).toBe('2x+');
  });
});

describe('formatSaveData', () => {
  it('returns expected strings', () => {
    expect(formatSaveData(SaveData.Off)).toBe('Off');
    expect(formatSaveData(SaveData.On)).toBe('On');
  });
});

describe('formatEncoding', () => {
  it('returns expected strings', () => {
    expect(formatEncoding(TransferEncoding.Identity)).toBe('Identity');
    expect(formatEncoding(TransferEncoding.Gzip)).toBe('Gzip');
    expect(formatEncoding(TransferEncoding.Brotli)).toBe('Brotli');
    expect(formatEncoding(TransferEncoding.Reserved)).toBe('Reserved');
  });
});

describe('formatMask', () => {
  it('formats 0x08 as Desktop / Original / 1x / Identity', () => {
    expect(formatMask(0x08)).toBe('Desktop / Original / 1x / Identity');
  });

  it('formats 0x0A as Desktop / AVIF / 1x / Identity', () => {
    expect(formatMask(0x0a)).toBe('Desktop / AVIF / 1x / Identity');
  });

  it('formats 0x11 as Mobile / WebP / 2x+ / Identity', () => {
    expect(formatMask(0x11)).toBe('Mobile / WebP / 2x+ / Identity');
  });

  it('formats 0x88 as Desktop / Original / 1x / Brotli', () => {
    expect(formatMask(0x88)).toBe('Desktop / Original / 1x / Brotli');
  });

  it('formats 0x00 as Mobile / Original / 1x / Identity', () => {
    expect(formatMask(0x00)).toBe('Mobile / Original / 1x / Identity');
  });
});

describe('formatBytes', () => {
  it('formats 0 bytes', () => {
    expect(formatBytes(0)).toBe('0 B');
  });

  it('formats small byte values', () => {
    expect(formatBytes(512)).toBe('512 B');
    expect(formatBytes(1)).toBe('1 B');
    expect(formatBytes(1023)).toBe('1023 B');
  });

  it('formats kilobytes', () => {
    expect(formatBytes(1024)).toBe('1.0 KB');
    expect(formatBytes(1536)).toBe('1.5 KB');
    expect(formatBytes(10240)).toBe('10.0 KB');
  });

  it('formats megabytes', () => {
    expect(formatBytes(1048576)).toBe('1.0 MB');
    expect(formatBytes(3355443)).toBe('3.2 MB');
  });

  it('formats gigabytes', () => {
    expect(formatBytes(1073741824)).toBe('1.0 GB');
  });

  it('formats terabytes', () => {
    expect(formatBytes(1099511627776)).toBe('1.0 TB');
  });
});

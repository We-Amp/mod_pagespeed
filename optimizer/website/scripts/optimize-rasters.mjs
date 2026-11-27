#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Hand-generate AVIF + WebP siblings for raster PNGs the runtime pipeline
// would otherwise serve as `x-pagespeed: MISS`. We dogfood, but we don't
// ship raw PNG to first paint while the cache is cold.
//
// Usage: node scripts/optimize-rasters.mjs <png> [<png> ...]
import sharp from 'sharp';
import { statSync } from 'node:fs';
import { extname, basename } from 'node:path';

const inputs = process.argv.slice(2);
if (inputs.length === 0) {
  console.error('Usage: node scripts/optimize-rasters.mjs <png> [<png> ...]');
  process.exit(1);
}

const fmt = (n) => (n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n} B`);

for (const input of inputs) {
  if (extname(input).toLowerCase() !== '.png') {
    console.warn(`skip (not .png): ${input}`);
    continue;
  }
  const base = input.slice(0, -4);
  const webpOut = `${base}.webp`;
  const avifOut = `${base}.avif`;

  const pngSize = statSync(input).size;

  // WebP: quality 78 is a sweet spot for screenshots — sharp edges, UI text.
  await sharp(input).webp({ quality: 78, effort: 6 }).toFile(webpOut);
  // AVIF: quality 50 + effort 6. AOM encoder via libheif/aom-3.13.1.
  await sharp(input).avif({ quality: 50, effort: 6 }).toFile(avifOut);

  const webpSize = statSync(webpOut).size;
  const avifSize = statSync(avifOut).size;
  const webpPct = ((webpSize / pngSize) * 100).toFixed(1);
  const avifPct = ((avifSize / pngSize) * 100).toFixed(1);

  console.log(
    `${basename(input)}: PNG ${fmt(pngSize)} -> WebP ${fmt(webpSize)} (${webpPct}%) -> AVIF ${fmt(avifSize)} (${avifPct}%)`,
  );
}

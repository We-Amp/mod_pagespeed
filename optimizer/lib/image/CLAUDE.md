# Image Processing Pipeline

Pipeline order: Decode → Content Analysis → Resize → Encode
→ SSIMULACRA2 check (JPEG only) → Re-encode if needed.

Safety limit: `kMaxDecodedPixels` (50MB) for decoded pixel buffers.

## Content-Aware Quality (`content_analyzer.h`)

Classifies images and adjusts quality per-class. Disable: `--no-content-analysis`.

| Class | Factor | Description |
|-------|--------|-------------|
| kPhoto | 1.0 | Natural photograph (default) |
| kScreenshot | 1.15 | UI/screenshot (sharp edges, viewport >= 768px only) |
| kIllustration | 1.10 | Vector-like art / low color count |
| kNoisy | 0.85 | High noise (compress more aggressively) |

Analysis stages: (1) unique color sampling via LCG PRNG, (2) edge density via
`SobelGradient()` + `PhotoMetric()`, (3) noise estimation via 8x8 patch variance MAD.

## Viewport-Based Resizing

Uses `ScanlineResizer` (area-based downscaler ported from mod_pagespeed).
Mobile=480px, Tablet=768px, Desktop=0 (no resize). 2x+ density doubles target width.
Aspect ratio preserved. Static GIFs resized; animated GIFs skip resize.

**API**: `TranscodeMultiResized(input_data, formats, viewport, density, save_data)`
— decodes once, resizes if wider than target, encodes to all requested formats.

## SSIMULACRA2 Quality Verification

Perceptual quality check after JPEG encoding. Re-encodes if score outside target
range (default: 70 +/- 5.0). Only JPEG triggers re-encode; WebP/AVIF logged only.
Disable: `--no-quality-verify`.

Config: `--target-ssimulacra2 SCORE`, `--ssimulacra2-tolerance F`.

Score range: 90+ imperceptible, 70-90 high quality, 50-70 acceptable, <50 artifacts.

Dependencies: `@skcms` (Skia CMS), `@jpegli//:ssimulacra2`.

## Save-Data Quality Overrides

When save-data=on: WebP 50 (vs 75), AVIF 45 (vs 60), JPEG 60 (vs 85).
Configured via `ImageTranscoderConfig::savedata_*_quality`.

## ECT Connection Quality

Quality multiplied by ECT factor for non-4G+ connections (3G: 0.85, 2G: 0.70,
Slow-2G: 0.50). AVIF uses the same multiply direction as JPEG/WebP (0-100,
higher = better). Proactive loop does NOT iterate connection types.

## SVG Candidacy Detection (`content_analyzer.h`)

`EvaluateSvgCandidacy()` scores raster images for vectorization suitability.
Multi-signal scoring on the decoded pixel buffer:

| Signal | Score | Description |
|--------|-------|-------------|
| Quantized unique colors <= 16 | +40 | 5-bit bucketing collapses anti-aliasing |
| Quantized unique colors <= 64 | +30 | |
| Quantized unique colors <= 256 | +15 | |
| Flat region ratio > 0.7 | +25 | Uniform regions (logo/icon indicator) |
| Flat region ratio > 0.5 | +12 | |
| Alpha coverage > 0.2 | +15 | Transparency = vector-origin content |
| Edge density + low photo metric | +10 | Geometric edges |
| Low photo metric (< 8.0) | +10 | Computer-generated content |
| PNG with alpha | +5 | Format hint |
| GIF source | +3 | Format hint |
| PNG metadata SVG-origin tool | +20 | Inkscape/Figma/Illustrator fingerprint |
| Multi-signal bonus | +15 | Colors <= 64 AND flat > 0.7 AND alpha > 0.2 |

Hard rejects: `kPhoto`/`kNoisy` content class, pixel count > `svg_max_pixels`,
dimensions < 16px, aspect ratio outside 0.25-4.0, animated images.

Score >= threshold (default 50) returns `kAttempt`; otherwise `kReject`.

## Pixel Preprocessing (`svg_preprocessor.h`)

Applied before vectorization to improve output quality:

1. **Median-cut color quantization** -- 5-bit channel bucketing, reduces to
   `min(quantized_unique_colors, 32)` dominant colors. Collapses anti-aliasing
   fringes into base colors.
2. **Binary alpha thresholding** -- Snaps alpha to 0 or 255 at threshold 128.
   Eliminates semi-transparent paths that inflate SVG size.
3. **Optional morphological close** -- 1px radius dilate-then-erode. Fills
   anti-aliasing gaps in thin features. Applied only for alpha images with
   quantized_unique_colors <= 16.

Cost: O(n) per step, negligible compared to vectorization.

## SVG Vectorization (`svg_vectorizer.h`)

Wraps VTracer (Rust) via FFI for raster-to-SVG conversion.

Pipeline: preprocess pixels -> VTracer FFI (`vtracer_convert`) -> sanitize
-> path count gate -> size gate.

- **Adaptive color precision**: `quantized_unique_colors <= 16` -> precision 4;
  `<= 64` -> precision 5; `<= 256` -> precision 6.
- VTracer FFI built as Rust staticlib via `rules_rust`. C API in
  `tools/vtracer-ffi/src/lib.rs`. Rust side validates all pointers, buffer
  sizes (overflow-safe `u64` arithmetic), and clamps parameters.
  Built with `panic = "abort"` and system allocator.
- **Preset-based geometry**: `--svg-preset` (0=bw, 1=poster, 2=photo) controls
  VTracer corner_threshold, segment_length, splice_threshold, and mode.
- Vectorization timeout: 500ms per image (configurable via `--svg-timeout-ms`).
  Post-hoc gate: discards result if elapsed time exceeds threshold.
- **Fidelity gate** (`--svg-fidelity-threshold`, default 55.0): After vectorization,
  re-rasterizes SVG via nanosvg, computes SSIMULACRA2 vs original pixels,
  rejects if score is below threshold. Uses `svg_rasterizer.h` for rasterization.

## SVG Rasterizer (`svg_rasterizer.h`)

Re-rasterizes SVG output to RGBA pixel buffers for fidelity verification.
Uses nanosvg (header-only, zlib license) for SVG parsing and rasterization.
`ConvertToRGBA()` helper normalizes 1/3/4 bpp input to RGBA for comparison.

## SVG Sanitizer (`svg_sanitizer.h`)

Allowlist-based structural validator for generated SVG output.

**14 allowed elements**: `svg`, `path`, `rect`, `circle`, `ellipse`, `line`,
`polyline`, `polygon`, `g`, `defs`, `clipPath`, `linearGradient`,
`radialGradient`, `stop`.

**12 dangerous elements stripped with subtrees**: `script`, `style`,
`foreignObject`, `use`, `animate`, `set`, `a`, `metadata`, `image`,
`handler`, `iframe`, `embed`. Also strips all `on*` event attributes,
`href`/`xlink:href`, DOCTYPE/ENTITY declarations, and comments.

Post-processing: coordinate rounding (1 decimal), explicit `width`/`height`
on root `<svg>`, whitespace collapse, empty `<g>` removal,
`shape-rendering="crispEdges"` for images with <= 16 quantized colors.

No external XML library dependency -- lightweight tag-level parser sufficient
for VTracer's known output vocabulary.

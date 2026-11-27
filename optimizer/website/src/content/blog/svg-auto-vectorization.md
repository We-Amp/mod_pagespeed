---
title: 'Auto-vectorizing raster images to SVG: one variant for all'
description: 'Raster to SVG auto-vectorization in mod_pagespeed: the optimizer worker traces logos and icons with VTracer, collapsing the variant matrix into one.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['images', 'image-optimization', 'performance', 'deep-dive', 'caching']
draft: false
product: '2.0'
---

A site logo arrives as `logo.png`: 8.2 KB, 240×80 pixels, three flat colors on a transparent background. To cover a Retina display it ships a 480×160 `@2x` version too. To look right on a phone there is a smaller crop. Each of those is its own cache entry, its own download, and its own blurry compromise at any DPI the designer did not anticipate. The same content as a Brotli-compressed SVG is around 600 bytes, renders crisply at any resolution, and is a single cache entry.

That gap is the whole point of the SVG pipeline in the optimizer worker. For images that are genuinely simple — logos, icons, flat illustrations — vectorizing removes them from the raster variant matrix entirely. There is nothing left to pick a viewport class for, no `@1x`/`@2x` pair, no per-density encode. One resolution-independent file serves every client. This post covers how 2.0 decides which images qualify and how it produces and serves them without breaking the ones that do not.

## Detect first, vectorize second

Vectorizing the wrong image is worse than not vectorizing at all. A photograph traced to SVG can be ten to a hundred times *larger* than the source JPEG, and a brand logo with a botched color cluster is a visible defect on every page it appears. So the feature ships in detect-first mode, controlled by `--svg-mode`. The default is `detect`: the worker classifies every image for SVG candidacy and surfaces the result in the workbench, but it does not vectorize anything. `preview` adds vectorization and stores the result for workbench inspection without serving the SVG to clients. `auto` is the only mode that serves SVG, and even then only when a vectorized candidate clears the size gate against its raster alternates.

The classifier reuses signals the [content analyzer](/blog/self-hosted-image-optimization/) already computes on the decoded pixel buffer — unique color count, photo metric, edge density, [content class](/blog/image-content-classification-and-denoise/) — and adds a few cheap single-pass heuristics. The most important one is **flat region ratio**: the fraction of pixels whose color matches their right and bottom neighbor. Logos and icons are mostly uniform fills (ratio above 0.7); photographs are continuous gradient (below 0.4). Colors are quantized to 5 bits per channel before comparison, which matters because JPEG compression adds per-channel noise of a few levels in flat areas — quantizing to a step size of 8 absorbs that noise so a JPEG-compressed logo still reads as flat.

Other signals: significant transparency (alpha coverage above 20%) points to vector-origin content; a low photo metric and a PNG-with-alpha source both add a few points; aspect ratios outside 0.25–4.0 are rejected because banners and panoramas rarely vectorize well. These feed a 0–100 score. Photos, noisy content, images under 16×16, and anything above the configured pixel ceiling are hard-rejected before scoring even runs. The launch ceiling is conservative — `svg_max_pixels` defaults to 65536, i.e. 256×256 — and is configurable upward. Score 50 or above means "attempt." The threshold itself is tunable via `--svg-candidacy-threshold`.

This is a deliberately small population. On a typical site, 5–10% of images are SVG candidates. Vectorization is one line in [the 2.0 feature set](/features/), not the headline. But for the images it does catch, the win is large and structural.

## VTracer, with the sharp edges sanded off

Detection decides *whether*; vectorization decides *how well*. The engine is [VTracer](https://github.com/visioncortex/vtracer), an MIT/Apache-licensed Rust library that does full-color clustering in linear time and needs no GPU. It is built from source inside Bazel's sandbox through a thin FFI crate (a `staticlib` linked as a `cc_library`) rather than vendored as an opaque binary — the same hermetic-build posture 2.0 already uses for its other native image dependencies — with `Cargo.lock` pinned. The FFI boundary is built defensively: `panic = "abort"` so a Rust panic cannot become undefined behavior across the boundary, null and dimension checks on both sides of the call, and SVG output buffers that the Rust side allocates are returned to it through a paired `vtracer_free` so there is no allocator mismatch with C++.

VTracer has known limitations, and the pipeline compensates for them rather than pretending they are not there. Its color clustering is uniform, not perceptual, so a **pixel preprocessing** stage runs first: median-cut color quantization snaps every pixel to one of the dominant colors (capped at 32), which collapses anti-aliasing fringes into their base colors instead of letting them spawn spurious thin paths; alpha is thresholded to binary at 128 to avoid the file-size bloat and cross-browser inconsistency of partial transparency. VTracer also emits only `<path>` elements — no native `<circle>` or `<rect>` — so a shape that would be a one-line primitive becomes a multi-point path. The size gate (below) is what catches the cases where that produces bloat.

Then the **size gate**, which only serves the SVG when it actually wins. It compares the *uncompressed* SVG against the smallest raster variant produced across every viewport/density/Save-Data combination, and serves the SVG only if it is smaller. Uncompressed is the deliberate choice: an identity-encoding client (cURL, some API consumers, anything with no `Accept-Encoding`) gets the raw SVG, which can be several times larger than its Brotli form, so the baseline comparison has to be safe for that worst case. Two more limits sit inside the vectorizer itself: a path-count gate (default 500, `--svg-max-paths`) and a 256 KB uncompressed cap (`--svg-max-svg-bytes`), both there to stop a pathological SVG from becoming a client-side render bomb. A perceptual **fidelity gate** — re-rasterizing the SVG and scoring it with SSIMULACRA2 against the original, with a relaxed threshold to allow for the anti-aliasing changes vectorization legitimately introduces — is designed but not yet wired into the serving path; its `--svg-fidelity-threshold` flag is reserved.

Output is run through an allowlist sanitizer before it is ever stored — known-good elements and attributes in, everything else stripped: no `<script>`, no `<foreignObject>`, no `href`, no event handlers, no DOCTYPE or entity declarations. Because VTracer's output vocabulary is tiny and known, an allowlist covers all legitimate output while closing the SVG XSS surface, and served SVGs additionally carry `Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'` and `X-Content-Type-Options: nosniff` as defense in depth.

## Raster-to-SVG auto-vectorization: collapsing the variant matrix

This is where vectorization differs from every other image optimization 2.0 does, and why it earns its own pipeline. Optimized JPEG, WebP, and [AVIF](/blog/avif-vs-webp-2026/) are all *raster*: each one still multiplies out across viewport class, pixel density, and Save-Data state. 2.0's [variant-aware cache](/blog/viewport-aware-image-optimization/) keys those alternates by a capability bitmask and the [selector](/how-it-works/async-rewriting/) picks the closest match per request. An SVG is resolution-independent, so none of those dimensions apply. The worker stores **one** SVG per URL — plus its gzip and Brotli pre-compressed forms — and that single variant serves desktop, tablet, and mobile, at 1x and 2x-or-higher density, alike. One file replaces up to six raster permutations.

Making the selector actually choose it took deliberate scoring changes, since an SVG stored at the desktop/1x/identity mask would otherwise score zero against a client asking for WebP/mobile/2x and never get picked. So `kSvg` gets a universal format bonus that beats an exact raster format match, and the viewport and density bonuses are always awarded to it because those dimensions are meaningless for a resolution-independent file. One structural guard keeps the format bits correct: `kSvg` is never derived from a client request header — the `Accept`-header parser only ever resolves to AVIF, WebP, or the original format, so SVG exists strictly as a worker-written stored alternate. When `Save-Data: on`, SVG gets an additional bonus, since by the size gate's definition it is the smaller option. The serving path also matters for layout: post-processing forces an explicit `width`/`height` on the root `<svg>` matching the source raster dimensions, so substituting the SVG does not shift the page and cost you [CLS](/core-web-vitals/cls/).

The decode-once discipline is what keeps this cheap. The image is decoded a single time at original resolution, analyzed once, and vectorized once — hoisted *before* the proactive raster loop runs, with the SVG result compared against the smallest raster only after that loop completes. There is no redundant work and the candidacy decision stays consistent across variants.

One caveat: SVG render cost scales with path count, not pixel count, and for an LCP element that path tessellation can regress paint timing. So `--svg-exclude-lcp` defaults to `true` — the worker skips vectorization for the detected LCP image and lets an optimized raster serve it, since raster decodes faster. You can flip it off for sites whose LCP element is a clean, low-path logo. That trade-off, and where it lands for your pages, is worth measuring against your [LCP](/core-web-vitals/lcp/) baseline.

## Related

- [Why self-hosted image optimization beats a CDN black box](/blog/self-hosted-image-optimization/)
- [Viewport-aware image optimization and the variant matrix](/blog/viewport-aware-image-optimization/)
- [How 2.0 classifies image content before it optimizes](/blog/image-content-classification-and-denoise/)
- [Serving lighter images under Save-Data](/blog/save-data-bandwidth/)
- [The economics of image optimization](/blog/economics-of-image-optimization/)
- [AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/)
- [Cumulative Layout Shift](/core-web-vitals/cls/)

Raster-to-SVG auto-vectorization ships in detect-first mode, so the safe path is to run it that way: point the optimizer worker at your site, let it score your images, and read the workbench to see which logos and icons it flags and what they would save before a single byte changes for a visitor. When you are ready, switch `--svg-mode` to `auto` and let the SVG collapse the variant matrix for the assets that qualify. Grab a build from the [downloads page](/download/) and read the [configuration reference](/docs/configuration/) for the `--svg-*` flags. It is licensed under Apache-2.0 and free to run in development and in production, so you can evaluate whether vectorization is worth it on your own images.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

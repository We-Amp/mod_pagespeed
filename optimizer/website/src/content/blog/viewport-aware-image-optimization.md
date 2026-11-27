---
title: 'Automatic WebP/AVIF on nginx: one decode, 37 variants'
description: 'Serve WebP and AVIF automatically on nginx, self-hosted. The mod_pagespeed worker decodes each image once and builds up to 37 responsive variants.'
date: 2026-02-03
lastUpdated: 2026-10-25
author: 'Otto van der Schaaf'
tags: ['architecture', 'images', 'performance', 'nginx']
product: '2.0'
draft: false
---

## The problem with one-at-a-time transcoding

The traditional approach to image optimization is reactive: a user requests an image, the server detects the client's capabilities, transcodes the image on the fly or from a queue, and caches the result. The next user with different capabilities -- different browser, different screen size, different network -- triggers another transcode. Each transcode decodes the source image from scratch, applies the transformation, and encodes the output.

For a 10-megapixel JPEG photograph, decoding produces roughly 40 MB of raw pixel data. If your site serves three image formats (WebP, AVIF, optimized JPEG), three viewport classes (mobile, tablet, desktop), two pixel densities (1x, 2x), and two Save-Data modes (on, off), you have a matrix of 3 x 3 x 2 x 2 = 36 possible raster variants per image (up to 37 with SVG auto-vectorization for qualifying images). The one-at-a-time approach means up to 37 independent decode passes -- 1.4 GB of pixel data decompressed and discarded, 36 times, for a single source image.

This is not a theoretical concern. On a site with 500 product images, each at an average resolution of 3000x2000 pixels, the reactive approach consumes 18,000 decode operations across the full variant matrix. Each decode allocates roughly 18 MB of pixel data (3000 x 2000 x 3 bytes). The total memory churn -- allocated, written, read during encoding, then freed -- exceeds 300 GB over the lifetime of the variant matrix. The CPU time for JPEG decoding alone (at roughly 20 ms per image) adds up to 6 minutes. And this is before encoding, which is substantially more expensive than decoding for codecs like AVIF.

The optimizer worker takes a different approach. When it receives the first notification for an image URL, it decodes the source once, then generates all missing variants from that single pixel buffer. One decode pass, one resize per viewport, and one encode per output format. The 1.4 GB of redundant decompression drops to 40 MB. The decode CPU time drops by a factor proportional to the number of variants sharing each decode pass. This work runs [off the request path](/how-it-works/async-rewriting/), so a visitor is never blocked on a transcode.

## The 32-bit capability mask

Every HTTP request that arrives at the nginx interceptor is classified into a 32-bit integer called the capability mask. This mask captures what the client supports and what it prefers, derived from standard HTTP headers.

The bit layout is compact:

```
Bits 0-1:  Image Format   (00=Original, 01=WebP, 10=AVIF, 11=SVG)
Bits 2-3:  Viewport Class (00=Mobile, 01=Tablet, 10=Desktop)
Bit  4:    Pixel Density  (0=1x, 1=2x+)
Bit  5:    Save-Data      (0=off, 1=on)
Bits 6-7:  Transfer Enc.  (00=Identity, 01=Gzip, 10=Brotli, 11=Reserved)
```

Eight bits encode the full range of client variation that matters for image serving. The remaining 24 bits are reserved for future dimensions -- progressive enhancement levels, color gamut preferences, or whatever the next decade of client diversity brings.

The mask is constructed from HTTP headers. The `Accept` header reveals format support: `image/avif` means the client can decode AVIF, `image/webp` means WebP. The `User-Agent` header determines viewport class via mobile, tablet, and desktop indicators. The `Save-Data` header, when set to `on`, signals the user has opted for reduced data consumption. The `Accept-Encoding` header determines transfer encoding preference (brotli, gzip, or identity) for pre-compressed text variants.

The cache lookup is then straightforward: the URL and hostname are [hashed to find the cache entry](/blog/cache-key-derivation-and-alternate-fallback/), and the mask selects the best-fit alternate. A request for `/images/hero.jpg` from a mobile Chrome browser uses mask `0x01` -- encoding WebP format, Mobile viewport, 1x density, Save-Data off, Identity transfer encoding -- to pick the right variant from the stored alternates.

The default capability mask -- used when nginx stores the original content on a cache miss -- is `0x08`. This corresponds to Desktop, Identity, Original format, 1x density, Save-Data off. It is the identity configuration: the most common client type on the open web, which receives the content in its original form before the worker has optimized it.

One subtlety: `CapabilityMask::Decode(0)` is not the same as the default constructor. Decoding zero produces Mobile, Identity, Original, 1x, Save-Data off. The default constructor produces Desktop, Identity. This matters when reasoning about the fallback chain and cache key semantics -- mask zero is the most constrained client, not the most common one.

## Why a bitmask instead of a hash

An alternative design would hash the relevant headers into a cache key. This would be simpler to implement -- no explicit bit layout, no enum definitions, just concatenate the header values and hash. But a bitmask has three advantages that matter for a cache system.

First, fallback is computable. Given a mask, the cache selector can score all stored alternates by mask similarity and pick the best fit in a single pass. With a hash, you would need to reconstruct the original header values, modify them, and re-hash -- which requires storing the original headers or maintaining a mapping table.

Second, the mask is small and constant-size. A single byte (the low 8 bits) serves as the alternate identifier. The full 32-bit mask is stored in per-alternate metadata for scoring.

Third, the mask is human-readable for debugging. A hex value like `0x09` can be decoded by hand: bits 0-1 = 01 (WebP), bits 2-3 = 10 (Desktop), bit 4 = 0 (1x), bit 5 = 0 (no Save-Data), bits 6-7 = 00 (Identity). When investigating cache behavior, this is invaluable compared to opaque hashes.

## The variant matrix

The full variant matrix for a single image URL has up to 37 entries when all proactive dimensions are enabled (36 raster variants plus an optional SVG auto-vectorization for qualifying images):

| Dimension                | Values                                                | Count        |
| ------------------------ | ----------------------------------------------------- | ------------ |
| Image Format             | WebP, AVIF, Optimized Original                        | 3            |
| Viewport Class           | Mobile (480px), Tablet (768px), Desktop (original)    | 3            |
| Pixel Density            | 1x, 2x+                                               | 2            |
| Save-Data                | Off, On                                               | 2            |
| **Raster total**         |                                                       | **36**       |
| SVG (auto-vectorization) | For qualifying images (logos, icons, simple graphics) | **+1**       |
| **Total**                |                                                       | **up to 37** |

SVG is the auto-vectorization format -- the worker generates resolution-independent [SVG variants](/blog/svg-auto-vectorization/) for suitable raster images (simple graphics, logos, icons) using VTracer via Rust FFI. The transfer encoding dimension (bits 6-7) is not included in image variant generation because images are already in compressed formats -- gzipping a WebP or AVIF is counterproductive. Transfer encoding variants are produced for text resources (HTML, CSS, JS) only.

Generating both WebP and AVIF in the same pass sidesteps the usual "which one should I serve?" debate -- the capability mask picks the best format the client actually supports. For the trade-offs behind serving both, see [AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/).

Not all 37 variants are equally likely to be requested. Desktop/1x/SaveData-off visitors dominate on most sites. The proactive system generates all variants eagerly because the marginal cost of encoding one more format from an already-decoded, already-resized pixel buffer is small compared to the cost of decoding the source image again later when a different client type arrives.

## How TranscodeMultiResized works

The core of the proactive system is `ImageTranscoder::TranscodeMultiResized()`. Here is what happens when the worker processes an image notification:

**Step 1: Decode.** The source image (JPEG, PNG, GIF, or WebP) is decoded to a raw pixel buffer -- an array of RGB or RGBA values at the image's native resolution. For a 4000x3000 JPEG at 3 bytes per pixel, this is 36 MB of pixel data. A safety limit (`kMaxDecodedPixels`, 50 MB) prevents out-of-memory conditions from adversarial or extremely large images.

**Step 2: Resize.** If the viewport class has a target width configured, the pixel buffer is downscaled using the `ScanlineResizer`, an area-based downscaler ported from the original mod_pagespeed. Mobile defaults to 480 pixels wide, tablet to 768 pixels wide, and desktop passes through at original resolution. Aspect ratio is always preserved. For 2x pixel density, the target width is doubled -- mobile becomes 960px, tablet 1536px -- to provide sharp rendering on high-DPI screens while still being smaller than the full-resolution original.

If the image is already narrower than the target width, no resize occurs. GIF input bypasses the resize path entirely and is routed to the animated-GIF-to-WebP pipeline.

**Step 3: Encode.** The (possibly resized) pixel buffer is encoded into each requested output format. WebP uses lossy encoding at quality 75 (or 50 with Save-Data). AVIF uses the libaom encoder at the default `avif_quality` of 60 and speed 6 (or 45 with Save-Data), on a 0-100 scale where higher is better. The optimized original re-encodes JPEG at quality 85 with progressive scanning, or runs PNG through optipng for lossless reduction. Each encode produces an independent output buffer.

**Step 4: Write.** Each output buffer is written to the Cyclone cache as an alternate identified by the appropriate capability mask. A Mobile/WebP/1x/SaveData-off variant gets one alternate ID, a Tablet/AVIF/2x/SaveData-on variant gets another. The variant writer checks whether each alternate already exists before writing, so re-processing the same image is idempotent.

The separation of decode, resize, and encode is critical for performance. Decoding is memory-intensive (it allocates the full pixel buffer) but CPU-light. Resizing is moderately expensive -- the area-based downscaler reads every pixel in the source buffer and writes every pixel in the destination buffer, which is essentially a full-image convolution. Encoding is CPU-intensive and varies dramatically by codec: JPEG encoding at quality 85 takes 5-15 ms for a 480px-wide image, WebP takes 10-30 ms, and AVIF at speed 6 takes 50-200 ms. By sharing the decode and resize work across all format encodings, the system amortizes the two cheaper steps and only pays the expensive encoding step once per format.

## The proactive loop

The proactive generation is orchestrated by a four-level nested loop in the worker's notification handler. The loop iterates over all enabled dimensions and, for each combination, builds the list of formats that do not yet exist in cache:

```
for each save_data in [Off, On]:           // outer: bandwidth preference
  if save_data != requested and proactive_savedata disabled: skip

  for each density in [1x, 2x+]:           // pixel density
    if density != requested and proactive_density disabled: skip

    for each viewport in [Mobile, Tablet, Desktop]:  // viewport class
      if viewport != requested and proactive_viewport disabled: skip

      missing_formats = []
      for each format in [WebP, AVIF, Original]:
        mask = build_mask(format, viewport, density, save_data)
        if not cache.exists(url, mask):
          missing_formats.append(format)

      if missing_formats is empty: continue

      results = TranscodeMultiResized(image_data, missing_formats,
                                      viewport, density, save_data)
      write each successful result to cache
```

The key optimization is that `TranscodeMultiResized` decodes and resizes once per viewport/density/save-data combination, then encodes to all missing formats from the same pixel buffer. When all dimensions are enabled, the worst case is 2 save-data x 2 density x 3 viewport = 12 calls to `TranscodeMultiResized`, each producing up to 3 format variants. But each call shares a decode pass across its formats, and in practice most calls find that some variants already exist and only need to produce one or two missing formats.

The loop structure also means that each dimension can be independently disabled:

- Without `--proactive-image-variants` (the default), the worker only generates the exact format requested by the triggering client. Passing `--proactive-image-variants` enables the full proactive system.
- `--no-proactive-viewport-variants` restricts viewport generation to the triggering client's viewport class. Format siblings are still generated.
- `--no-proactive-savedata-variants` skips Save-Data siblings. Only the triggering client's Save-Data preference is used.
- `--no-proactive-density-variants` skips pixel density siblings. Only the triggering client's density is used.

An operator who knows their traffic is 95% desktop can disable viewport siblings and cut the variant count by two-thirds without affecting the format optimization.

## Fallback when variants are missing

Not every variant will exist in cache at every moment. A freshly deployed system starts with an empty cache. A low-traffic URL may only have been requested by one client type. The cache might evict rarely-accessed variants under LRU pressure.

The nginx interceptor handles this with a fallback chain. When a cache lookup for the exact capability mask misses, the interceptor tries progressively degraded masks in a specific priority order:

1. **Downgrade pixel density.** Try the same format and viewport but at 1x density. A 1x image served to a 2x display looks slightly softer but loads faster.

2. **Downgrade image format.** Try AVIF -> WebP -> Original. The format chain ensures the client always gets something it can render, even if the preferred format has not been generated yet.

3. **Baseline fallback.** Original format, 1x density, preserving viewport and Save-Data. This is the most conservative variant.

4. **Default mask.** The original content recorded by nginx on the first cache miss. This always exists because nginx writes it on the initial proxy response.

The fallback chain means the system is eventually consistent. The first request for any URL always serves the original content (a cache "HIT" that happens to contain the original). The worker asynchronously generates optimized variants. Subsequent requests get progressively better variants as they become available. After the proactive loop completes, every client type gets its optimal variant on the next request.

## Performance impact

The numbers are straightforward. Consider a product catalog with 1,000 unique images, each averaging 200 KB as JPEG.

**Without proactive variants (one-at-a-time):**

Each unique client type that requests an image triggers a new decode + encode cycle. With up to 37 possible variants per image, the worst case is 37,000 decode passes. At 40 MB per decode, that is 1.4 TB of pixel data decompressed across the full variant matrix. Each decode takes 10-50 ms depending on image complexity, so the CPU time for decoding alone is 360-1800 seconds.

**With proactive variants (single decode per dimension set):**

Each image is decoded at most 12 times (one per viewport/density/save-data combination). That is 12,000 decode passes, producing 480 MB of pixel data each (after resize, typically 5-15 MB). Total CPU time for decoding drops to 120-600 seconds. More importantly, the encoding work happens immediately while the pixel buffer is still in memory, with no cache pollution from intermediate results.

**In practice, the savings are larger** because most images are not requested by all 37 client types. The proactive system front-loads the work: a single mobile/WebP notification triggers generation of up to 37 variants, and the next 36 client types find their variant already cached. The time-to-optimized for the long tail of client types drops from "whenever that client type first appears" to "a few hundred milliseconds after the first notification."

There is also a memory locality benefit. When you decode an image and immediately encode it to three formats, the pixel buffer is hot in the CPU cache. When you decode an image, discard the buffer, and decode it again minutes or hours later for a different format, the buffer starts cold. On modern hardware with large L3 caches, a 5-15 MB resized pixel buffer (mobile viewport) fits entirely in cache. The three sequential encoding passes (WebP, AVIF, JPEG) each read the buffer without any main memory stalls. This is a small but measurable improvement on image-heavy workloads.

## Deduplication and idempotency

The proactive system is safe against redundant work. Before calling `TranscodeMultiResized`, the loop checks whether each variant already exists in cache by calling `VariantExists()`. If all three formats already exist for a given viewport/density/save-data combination, that combination is skipped entirely. If two of three exist, only the missing format is requested.

This means that re-notifications for the same URL (which happen naturally -- nginx sends a notification on every cache fallback hit) do not trigger redundant work. The first notification produces the full variant matrix. Subsequent notifications find everything cached and return immediately. The overhead of the existence checks is negligible -- each is a single cache key lookup, which is a hash table operation on the memory-mapped directory.

Deduplication also applies at the notification level. Before the proactive loop even begins, the worker checks whether the specific variant requested by the notification already exists. If so, the notification is skipped entirely and the `notifications_skipped_dedup` counter is incremented. This fast path avoids the overhead of reading the source image from cache for notifications that arrive after the worker has already processed the URL.

## Hot URL warmup

For high-traffic URLs, the interceptor builds the full variant matrix ahead of demand instead of waiting for each client type to ask. nginx counts fallback hits per URL and, once a URL crosses `pagespeed_hot_threshold`, sends a warmup sentinel so the worker generates every missing variant before most visitors request one. The counter, the `0xFFFFFFFE` sentinel, and the `--enable-warmup` flag are covered in [proactive variant generation](/blog/proactive-variant-generation-cache-warmup/).

## Cache storage budget

The trade-off for proactive variant generation is cache storage. Generating up to 37 variants per image increases the cache footprint significantly compared to storing only the original.

For a 161 KB JPEG source image (1600x900 pixels, measured on 2026-10-25 at the shipped encoder defaults with proactive variant generation on):

| Variant                            | Measured Size |
| ---------------------------------- | ------------- |
| WebP (Desktop, 1x, normal)         | 36 KB         |
| WebP (Mobile 480px, 1x, normal)    | 23 KB         |
| WebP (Mobile 480px, 1x, Save-Data) | 16 KB         |
| AVIF (Desktop, 1x, normal)         | 46 KB         |
| AVIF (Desktop, 1x, Save-Data)      | 21 KB         |
| AVIF (Mobile 480px, 1x, normal)    | 20 KB         |
| AVIF (Mobile 480px, 1x, Save-Data) | 8 KB          |
| Optimized JPEG (Desktop, 1x)       | 54 KB         |
| Optimized JPEG (Mobile 480px, 1x)  | 25 KB         |

The nine measured variants of this image total about 250 KB, 1.5x the 161 KB original; the full 37-variant set adds the tablet and 2x density renditions, which were not measured here. For a 1,000-image catalog, the nine measured variants alone take roughly 250 MB of cache. The Cyclone cache handles this with LRU eviction: set `cache_size_bytes` to your available disk budget, and the least-recently-accessed variants get evicted first. Mobile/Save-Data/AVIF variants for a rarely-viewed product image will be evicted long before the Desktop/WebP variant of the hero banner.

The resized variants are where the real bandwidth savings come from. That 161 KB JPEG, served as a 480px-wide AVIF to a [mobile device with Save-Data enabled](/blog/save-data-bandwidth/), drops to 8 KB -- a 95% reduction. The storage cost per image is paid once on disk; the bandwidth saving of 153 KB per mobile page view is paid on every request. Because the variant matrix lives in your own Cyclone cache rather than a third-party CDN, those savings come without the [per-image processing fees or egress charges](/blog/economics-of-image-optimization/) of a third-party service -- one of the practical benefits of [keeping image optimization self-hosted](/blog/self-hosted-image-optimization/).

## Configuration reference

The proactive variant system is controlled by these worker flags:

| Flag                               | Default | Effect                                                                          |
| ---------------------------------- | ------- | ------------------------------------------------------------------------------- |
| `--proactive-image-variants`       | Off     | Enables multi-format proactive generation from a single decode pass             |
| `--no-proactive-viewport-variants` | Enabled | Restricts to requesting client's viewport; format siblings still generated      |
| `--no-proactive-savedata-variants` | Enabled | Skips Save-Data siblings                                                        |
| `--no-proactive-density-variants`  | Enabled | Skips pixel density siblings                                                    |
| `--mobile-width PIXELS`            | 480     | Target width for Mobile viewport (0 to disable resize)                          |
| `--tablet-width PIXELS`            | 768     | Target width for Tablet viewport (0 to disable resize)                          |
| `--desktop-width PIXELS`           | 0       | Target width for Desktop viewport (0 = no resize, serve at original resolution) |
| `--enable-warmup`                  | Off     | Enables hot URL variant warmup via warmup sentinels                             |

Nginx-side configuration:

| Directive                   | Default | Effect                                                               |
| --------------------------- | ------- | -------------------------------------------------------------------- |
| `pagespeed_hot_threshold N` | 5       | Fallback-hit count before sending warmup notification (0 to disable) |

The defaults are tuned for the common case: a site with mixed device traffic that benefits from having all variants ready. Operators with specific traffic profiles can narrow the variant matrix by disabling dimensions they do not need, reducing both CPU time and cache footprint proportionally.

## The ScanlineResizer: area-based downscaling

The resize step deserves a closer look because the choice of downscaling algorithm matters for image quality.

Naive image resizing (nearest-neighbor or bilinear interpolation) produces visible artifacts when downscaling by large factors. A 4000px-wide image resized to 480px with bilinear interpolation loses fine detail and can introduce moire patterns in high-frequency content like text overlays, fabric textures, and architectural detail.

The `ScanlineResizer`, ported from the original mod_pagespeed, uses area-based averaging. Each pixel in the output is the weighted average of all source pixels that fall within its area. For a 4000-to-480 downscale (factor of ~8.3x), each output pixel averages approximately 69 source pixels. This produces smooth, alias-free results equivalent to what Photoshop calls "Bicubic Sharper" or what ImageMagick calls "Area" resize.

The implementation operates on scanlines rather than the full pixel buffer. It reads one row of source pixels at a time, accumulates partial contributions to the current output row, and emits a completed output row when enough source rows have been consumed. This means the resizer never needs more than one row of source pixels and one row of output pixels in memory simultaneously -- important when processing images near the `kMaxDecodedPixels` safety limit.

For retina (2x density) resizing, the target width is doubled. Mobile at 2x becomes 960px, tablet at 2x becomes 1536px. This means a retina mobile user gets a 960px-wide image that is displayed at 480 CSS pixels -- four pixels per CSS pixel, producing sharp text and crisp edges on high-DPI screens. The file size is larger than the 1x variant but substantially smaller than the full-resolution desktop variant.

## Observability

The worker reports proactive variant generation through several counters accessible via the health check socket and the management STATS command:

- `variants_written`: Total variants written, including both directly-requested and proactive siblings.
- `proactive_variants_written`: Subset of `variants_written` that were generated proactively (not directly requested by the triggering notification).
- `notifications_skipped_dedup`: Notifications skipped because the requested variant already existed.
- Per-format counters (`webp_generated`, `avif_generated`, `jpeg_optimized`, `png_optimized`): Count of variants written per output format, regardless of whether they were proactive.

The ratio of `proactive_variants_written` to `variants_written` indicates how much work the proactive system is doing. A ratio near 0 means most variants are being generated reactively (proactive dimensions may be disabled, or traffic is dominated by a single client type). A ratio near 0.97 (36/37) means a single notification is populating the full matrix -- the ideal case.

The management socket also exposes timing data via the STATS command, including cumulative microseconds spent on image processing (`by_type.images.us`). Dividing this by `images_processed` gives the average per-image processing time, which includes decode, resize, and all format encoding passes.

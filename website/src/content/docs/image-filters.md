---
title: 'Image filters'
description: 'Image filters in mod_pagespeed for Apache, nginx and IIS: recompression, WebP and opt-in AVIF conversion, resizing, lazy loading and responsive images.'
order: 43
group: 'Filters'
lastUpdated: 2026-10-22
---

## Overview

mod_pagespeed includes image filters for recompression, format conversion, resizing, and inlining that cut page weight without touching the original files on disk. The master filter `rewrite_images` is a CoreFilter and enables several sub-filters by default. Enable additional image filters individually for finer control. The same filter names and directives apply across the in-process module on Apache, nginx, and IIS.

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```text
pagespeed EnableFilters rewrite_images
pagespeed ImageRecompressionQuality 75
```

See [IIS configuration](/docs/iis-configuration/) for the full file format reference.

## Quick reference

| Filter                                                                    | Core                 | OFB | Description                                                   | Safe          |
| ------------------------------------------------------------------------- | -------------------- | --- | ------------------------------------------------------------- | ------------- |
| [`rewrite_images`](#rewrite_images)                                       | Yes                  | -   | Master filter; enables recompress, resize, inline sub-filters | Yes           |
| [`recompress_images`](#recompress_images)                                 | via `rewrite_images` | Yes | Recompress and convert images (lossy re-encode)               | Yes           |
| [`recompress_jpeg`](#recompress_images)                                   | via `rewrite_images` | Yes | Recompress JPEG images                                        | Yes           |
| [`recompress_png`](#recompress_images)                                    | via `rewrite_images` | Yes | Recompress PNG images                                         | Yes           |
| [`recompress_webp`](#recompress_images)                                   | via `rewrite_images` | Yes | Recompress WebP images                                        | Yes           |
| [`convert_jpeg_to_progressive`](#recompress_images)                       | via `rewrite_images` | Yes | Convert large JPEGs to progressive encoding                   | Yes           |
| [`convert_jpeg_to_webp`](#convert_formats)                                | via `rewrite_images` | Yes | Serve WebP to capable browsers                                | Yes           |
| [`convert_png_to_jpeg`](#convert_formats)                                 | via `rewrite_images` | Yes | Convert opaque PNGs to JPEG                                   | Yes           |
| [`convert_gif_to_png`](#convert_formats)                                  | via `rewrite_images` | Yes | Convert GIF to PNG                                            | Yes           |
| [`convert_to_webp_animated`](#convert_formats)                            | No                   | No  | Convert animated GIF to animated WebP                         | Test first    |
| [`convert_to_webp_lossless`](#convert_formats)                            | via `rewrite_images` | No  | Lossless WebP for PNGs and GIFs classed as graphics           | Test first    |
| [`convert_jpeg_to_avif`](#avif)                                           | No                   | No  | Serve AVIF for photographic JPEG sources                      | Test first    |
| [`convert_to_avif_lossless`](#avif)                                       | No                   | No  | Serve lossless AVIF for PNG/GIF sources                       | Test first    |
| [`convert_to_avif_animated`](#avif)                                       | No                   | No  | Serve AVIF for animated sources                               | Test first    |
| [`recompress_avif`](#avif)                                                | No                   | No  | Re-encode AVIF images you already serve                       | Test first    |
| [`strip_image_color_profile`](#strip_metadata)                            | via `rewrite_images` | Yes | Remove ICC color profiles                                     | Yes           |
| [`strip_image_meta_data`](#strip_metadata)                                | via `rewrite_images` | Yes | Remove EXIF and other metadata                                | Yes           |
| [`jpeg_subsampling`](#recompress_images)                                  | via `rewrite_images` | Yes | Downsample JPEG color channels                                | Yes           |
| [`resize_images`](#resize_images)                                         | via `rewrite_images` | -   | Resize images to declared `width`/`height`                    | Yes           |
| [`resize_rendered_image_dimensions`](#resize_images)                      | No                   | -   | Resize images to rendered dimensions via JS                   | Test first    |
| [`inline_images`](#inline_images)                                         | via `rewrite_images` | -   | Inline small images as data: URIs                             | Yes           |
| [`responsive_images`](#responsive_images)                                 | No                   | No  | Generate `srcset` attributes                                  | Test first    |
| [`lazyload_images`](#lazyload_images)                                     | No                   | No  | Defer offscreen image loading                                 | Test first    |
| [`inline_preview_images`](#inline_images)                                 | No                   | No  | Show low-quality placeholder before full load                 | Test first    |
| [`resize_mobile_images`](#resize_images)                                  | No                   | No  | Serve smaller images to mobile devices                        | Test first    |
| [`dedup_inlined_images`](#inline_images)                                  | No                   | No  | Deduplicate repeated inlined images                           | Yes           |
| [`sprite_images`](#sprite_images)                                         | No                   | No  | Combine CSS background images into sprites                    | Test first    |
| [`insert_image_dimensions`](#resize_images)                               | No                   | No  | Add `width`/`height` to `<img>` tags                          | Test first    |
| [`in_place_optimize_for_browser`](#in_place_optimize_for_browser)         | No                   | No  | Retired: accepted with a warning, no effect                   | Retired       |
| [`extend_cache_images`](#extend_cache_images)                             | via `extend_cache`   | No  | Content-hashed image URLs with a one-year cache               | Yes           |
| [`responsive_images_zoom`](#responsive_images_zoom)                       | No                   | No  | Zoom-aware srcset selection for `responsive_images`           | Test first    |
| [`insert_img_dimensions`](#resize_images)                                 | No                   | No  | Alternate spelling of `insert_image_dimensions`               | Test first    |
| [`experiment_collect_mob_image_info`](#experiment_collect_mob_image_info) | No                   | No  | Mobilization experiment data collection                       | Dangerous set |

**Core** = enabled by default in the CoreFilters set.
**OFB** = enabled by OptimizeForBandwidth mode.

## Master filter: rewrite_images {#rewrite_images}

[Full guide →](/docs/filters/rewrite_images/)

### What it does

`rewrite_images` is the compound image filter: one name switches on the whole optimization pipeline, covering recompression, format conversion, resizing to declared dimensions, metadata stripping, and inlining of small images. Every `<img>` on the page, and every image referenced from CSS, is a candidate. If optimization finishes within the rewrite deadline, the first view already gets the `.pagespeed.ic.` URL; otherwise that view keeps the original and later views get the optimized one. Live demo: [rewrite_images](/examples/rewrite_images/).

```html
<!-- before -->
<img src="/photos/team.jpg" width="400" height="300" />

<!-- after, to a WebP-capable browser: recompressed, resized, cache-extended -->
<img src="/photos/400x300xteam.jpg.pagespeed.ic.HASH.webp" width="400" height="300" />
```

### When it helps and when it does not

The compound pays on sites whose images are uploaded as-is: photos exported at full resolution, opaque PNGs that would be far smaller as JPEG or WebP, files carrying camera metadata nobody reads. A site that already runs a disciplined image pipeline, with build-time resizing, modern formats, and stripped metadata, leaves it little to do, while the CPU cost of the first rewrite of every image still applies. It also does nothing for images on domains the configuration does not authorize, or for sources over `ImageResolutionLimitBytes` (default 33554432 bytes).

### How it decides

Each member filter applies its own test: resizing only happens when `width` and `height` are declared, WebP only when the browser advertises support, inlining only below `ImageInlineMaxBytes`. Every recompression or conversion is kept only when the result is smaller than the original, by the margin `ImageLimitOptimizedPercent` sets (default 100: any reduction). A losing rewrite is remembered, so a losing image is not re-encoded on every request. Some members are also part of OptimizeForBandwidth; the compound itself is a CoreFilter.

### Sub-filters enabled by default

When `rewrite_images` is active, the following sub-filters are enabled automatically:

- `recompress_images` (and its sub-filters: `recompress_jpeg`, `recompress_png`, `recompress_webp`)
- `convert_jpeg_to_progressive`
- `convert_jpeg_to_webp`
- `convert_png_to_jpeg`
- `convert_gif_to_png`
- `convert_to_webp_lossless`
- `strip_image_color_profile`
- `strip_image_meta_data`
- `jpeg_subsampling`
- `resize_images`
- `inline_images`

### Risks

- Recompression is lossy. The default quality levels are conservative, but check quality-critical imagery such as logos and screenshots with text after enabling.
- The first optimization of each image costs server CPU; `ImageMaxRewritesAtOnce` (default 8) bounds how many run in parallel on a cold cache.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-rewrite_images` comparison; [Is it working?](/docs/is-it-working/) has the steps.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters rewrite_images
```

**nginx:**

```nginx
pagespeed EnableFilters rewrite_images;
```

To disable a specific sub-filter while keeping `rewrite_images` active:

**Apache:**

```apache
ModPagespeedEnableFilters rewrite_images
ModPagespeedDisableFilters convert_jpeg_to_webp
```

**nginx:**

```nginx
pagespeed EnableFilters rewrite_images;
pagespeed DisableFilters convert_jpeg_to_webp;
```

## Recompression filters {#recompress_images}

<a id="recompress_jpeg"></a><a id="recompress_png"></a><a id="recompress_webp"></a>

[Full guide →](/docs/filters/recompress_images/) · Also: [`recompress_jpeg`](/docs/filters/recompress_jpeg/), [`recompress_png`](/docs/filters/recompress_png/), [`recompress_webp`](/docs/filters/recompress_webp/)

### What they do

The recompression filters reduce image file size by re-encoding images at an optimal quality level. They preserve visual quality while removing encoding inefficiencies, and each one handles exactly the format its name says, so a format you do not want re-encoded can be turned off by name without touching the others.

### How each format is recompressed

`recompress_jpeg` re-encodes JPEG images that reach it. It runs after the conversion candidates: a JPEG that was not converted to WebP or AVIF for this request is re-encoded at `JpegRecompressionQuality`, which defaults to -1, meaning it follows `ImageRecompressionQuality` (default 85). Only when `ImageRecompressionQuality` is itself -1 does the module keep the source file's own quality.

`recompress_jpeg` keeps a re-encode only when it is smaller than the original by the margin `ImageLimitOptimizedPercent` sets (default 100: any reduction at all), and a losing re-encode is remembered so the same JPEG is not re-encoded on every request. The filter is a CoreFilter through `rewrite_images` and also part of OptimizeForBandwidth; disabling it by name while keeping the compound on leaves the other formats recompressing.

`recompress_png` re-encodes PNG images losslessly. The PNG optimizer re-encodes the image and keeps the result only when it is smaller, under the same `ImageLimitOptimizedPercent` margin as the other formats; a losing result is remembered. This filter never makes a PNG lossy: turning a photographic PNG into a JPEG or a WebP is the conversion filters' decision, and when that decision does not apply or loses, the lossless re-encode this filter controls is what runs.

`recompress_png` is also the path a resized PNG takes, because resizing re-encodes the file. In OptimizeForBandwidth mode the smaller bytes replace the original response in place, with the URL unchanged. The filter is a CoreFilter through `rewrite_images`.

`recompress_webp` re-encodes WebP images the page already serves, at `WebpRecompressionQuality` (default 80). It does not create WebP; converting JPEG or PNG sources to WebP is the conversion filters' job. Animated WebP is never recompressed, and there is no fallback conversion of WebP to JPEG for browsers without WebP support: such sources are left alone rather than transcoded.

`recompress_webp` keeps a re-encode only when it is smaller under `ImageLimitOptimizedPercent`, and a losing result is remembered. A small-screen client gets a lower quality still: `WebpRecompressionQualityForSmallScreens` (default 70) applies to the requests the module classifies as small-screen. The filter is a CoreFilter through `rewrite_images` and part of OptimizeForBandwidth.

`recompress_images` is a convenience filter that enables `recompress_jpeg`, `recompress_png`, and `recompress_webp` together with `convert_gif_to_png`, `convert_jpeg_to_progressive`, `convert_jpeg_to_webp`, `convert_png_to_jpeg`, `jpeg_subsampling`, `strip_image_color_profile`, and `strip_image_meta_data`.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters recompress_images
# Or individually:
ModPagespeedEnableFilters recompress_jpeg,recompress_png,recompress_webp
```

**nginx:**

```nginx
pagespeed EnableFilters recompress_images;
# Or individually:
pagespeed EnableFilters recompress_jpeg,recompress_png,recompress_webp;
```

### Configuration

| Parameter                   | Default | Description                                                                       |
| --------------------------- | ------- | --------------------------------------------------------------------------------- |
| `ImageRecompressionQuality` | 85      | General quality level for recompressed images (-1 to 100; -1 uses source quality) |
| `JpegRecompressionQuality`  | -1      | JPEG-specific quality; -1 uses `ImageRecompressionQuality`                        |
| `WebpRecompressionQuality`  | 80      | WebP quality level                                                                |

**Apache:**

```apache
ModPagespeedImageRecompressionQuality 85
ModPagespeedJpegRecompressionQuality 75
```

**nginx:**

```nginx
pagespeed ImageRecompressionQuality 85;
pagespeed JpegRecompressionQuality 75;
```

### Risks

Recompression is lossy. Setting quality too low produces visible artifacts. The default values are conservative and safe for most content. Photographic content can tolerate lower quality than screenshots or text-heavy images.

## Format conversion filters {#convert_formats}

<a id="convert_jpeg_to_progressive"></a><a id="convert_jpeg_to_webp"></a><a id="convert_png_to_jpeg"></a><a id="convert_gif_to_png"></a><a id="convert_to_webp_animated"></a><a id="convert_to_webp_lossless"></a>

Full guides: [`convert_jpeg_to_progressive`](/docs/filters/convert_jpeg_to_progressive/), [`convert_jpeg_to_webp`](/docs/filters/convert_jpeg_to_webp/), [`convert_png_to_jpeg`](/docs/filters/convert_png_to_jpeg/), [`convert_gif_to_png`](/docs/filters/convert_gif_to_png/), [`convert_to_webp_animated`](/docs/filters/convert_to_webp_animated/), [`convert_to_webp_lossless`](/docs/filters/convert_to_webp_lossless/)

### What they do

Format conversion filters serve images in the most efficient format for each browser and image type:

- **`convert_jpeg_to_progressive`** converts large baseline JPEGs to progressive encoding. Progressive JPEGs render incrementally and are often smaller for images above ~10 KB.
- **`convert_jpeg_to_webp`** serves WebP versions of JPEG images to browsers that send `Accept: image/webp`. The original JPEG is preserved for other browsers.
- **`convert_png_to_jpeg`** converts PNG images with no alpha channel (fully opaque) that the image analysis classifies as photographic to lossy WebP or JPEG, which is typically much smaller for photographic content.
- **`convert_gif_to_png`** converts non-animated GIF images to PNG, which uses better compression.
- **`convert_to_webp_animated`** converts animated GIF images to animated WebP. Not a CoreFilter.
- **`convert_to_webp_lossless`** serves PNG and GIF images that the image analysis classifies as graphics (not photographic) as lossless WebP, which preserves every pixel. It does not make photographic PNGs lossless: those take the lossy path of `convert_png_to_jpeg` when that filter is on (the default). Without it, a PNG graphic stays PNG and a GIF graphic becomes PNG (with `convert_gif_to_png` on, the default). A CoreFilter, enabled by default through `rewrite_images`.

`convert_jpeg_to_webp` is the member of the family with the widest reach, because JPEG is where photographic weight usually sits. For a WebP-capable request, the filter encodes a WebP candidate at `WebpRecompressionQuality` (default 80) and keeps it when it is smaller than the original JPEG. WebP-capable requests (by `Accept: image/webp` or a known user agent) get HTML pointing at a `.webp` URL; others get the JPEG URL, so the HTML differs per browser; a CDN caching rewritten HTML must not share it across browsers. The filter runs in CoreFilters through `rewrite_images` and is listed in OptimizeForBandwidth, but in that level's in-place mode no WebP is selected.

`convert_png_to_jpeg` turns a PNG into a JPEG only when the module's image analysis says the file is photographic, meaning not sensitive to compression noise, and it has no transparent pixels. The analysis is what protects flat graphics: a logo or a diagram analyzes as non-photographic and keeps its PNG. On a photographic PNG without transparency the filter authorizes lossy encoding, and the format that ships depends on the browser: when the request prefers WebP and `convert_jpeg_to_webp` is on, the file becomes WebP rather than JPEG; otherwise it is re-encoded as JPEG at the JPEG quality setting. A PNG with transparency is never made a JPEG. The analysis is a heuristic, and a large, smooth photo (soft sky, landscape, a photo re-saved as PNG) can be classified as a graphic. It then takes the lossless path of `convert_to_webp_lossless` and ships as lossless WebP, which is much larger than a lossy encode. For photographic images, serve the source as JPEG, which the analysis always treats as photographic, or keep the original URL (`ImagePreserveURLs on`, see [`ImagePreserveURLs`](/docs/configuration/#imagepreserveurls), or `data-pagespeed-no-transform` on a single `<img>`) so the optimizer worker negotiates AVIF or lossy WebP, which gives smaller files. The cost is that the module's image inlining, resizing and lazy loading no longer apply to those images; the format rule is described in [Image format and the Accept header](/docs/cdn-integration/#image-format-negotiation). The converted file is kept only when it is smaller (`ImageLimitOptimizedPercent`), and a PNG carrying a C2PA content-credentials manifest is left untouched so its provenance chain survives. The filter runs in CoreFilters through `rewrite_images`.

`convert_gif_to_png` re-encodes a non-animated GIF as PNG. Without this filter, and without one of the animated-format conversions or the low-resolution preview path that need the same decode, a GIF that is not resized passes through the module untouched: no other filter re-encodes it. A resized GIF is decoded and resized as a PNG, so with this filter on it ships as PNG (or, if photographic and the lossy conversions apply, as JPEG or WebP); it never goes back out as a GIF. The conversion is lossless in intent (a GIF's at-most-256-color palette maps into PNG), and the result is kept only when it is smaller than the original GIF under `ImageLimitOptimizedPercent`. One non-obvious effect: the filter also unlocks lossy treatment of photographic GIFs, because the lossy decision path accepts a GIF only when it may first be treated as a PNG-style source. With the filter off, a photographic GIF is never turned into a JPEG or lossy WebP; it can still leave as lossless WebP or AVIF when those conversions are enabled, and otherwise stays a GIF. The filter runs in CoreFilters through `rewrite_images`.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters convert_jpeg_to_progressive
ModPagespeedEnableFilters convert_jpeg_to_webp
ModPagespeedEnableFilters convert_png_to_jpeg
ModPagespeedEnableFilters convert_gif_to_png
ModPagespeedEnableFilters convert_to_webp_animated
ModPagespeedEnableFilters convert_to_webp_lossless
```

**nginx:**

```nginx
pagespeed EnableFilters convert_jpeg_to_progressive;
pagespeed EnableFilters convert_jpeg_to_webp;
pagespeed EnableFilters convert_png_to_jpeg;
pagespeed EnableFilters convert_gif_to_png;
pagespeed EnableFilters convert_to_webp_animated;
pagespeed EnableFilters convert_to_webp_lossless;
```

### Configuration

| Parameter                          | Default | Description                                        |
| ---------------------------------- | ------- | -------------------------------------------------- |
| `ProgressiveJpegMinBytes`          | 10240   | Minimum JPEG size before converting to progressive |
| `WebpRecompressionQuality`         | 80      | Quality for lossy WebP conversion                  |
| `WebpAnimatedRecompressionQuality` | 70      | Quality for animated WebP conversion               |

### Risks

- `convert_png_to_jpeg` drops the alpha channel on opaque PNGs. mod_pagespeed checks for alpha before converting, but semi-transparent pixels at the boundary may cause subtle edge artifacts.
- `convert_jpeg_to_webp` decides per request, so the rewritten HTML names a `.webp` image for some browsers and a JPEG for others. A CDN or proxy that caches the HTML must not serve one browser's copy to another; test with your CDN configuration.
- `convert_to_webp_animated` can produce large files for complex animations. Verify output sizes.
- `convert_to_webp_lossless` applies to PNGs and GIFs the image analysis classes as graphics. A large, smooth photograph can be classed as a graphic and then served as lossless WebP, which can be much larger than a lossy encoding. Serve photographs as JPEG, or keep the original URL (`ImagePreserveURLs on`, or `data-pagespeed-no-transform`) so the optimizer worker negotiates a lossy format.

## AVIF filters {#avif}

<a id="convert_jpeg_to_avif"></a><a id="convert_to_avif_lossless"></a><a id="convert_to_avif_animated"></a><a id="recompress_avif"></a>

Full guides: [`convert_jpeg_to_avif`](/docs/filters/convert_jpeg_to_avif/), [`convert_to_avif_lossless`](/docs/filters/convert_to_avif_lossless/), [`convert_to_avif_animated`](/docs/filters/convert_to_avif_animated/), [`recompress_avif`](/docs/filters/recompress_avif/)

### What they do

Four filters transcode images to AVIF. Each one targets a different kind of source:

- **`convert_jpeg_to_avif`** encodes photographic JPEG sources as AVIF.
- **`convert_to_avif_lossless`** encodes flat-palette and screenshot-style sources (PNG, GIF) as lossless AVIF.
- **`convert_to_avif_animated`** encodes animated sources as AVIF.
- **`recompress_avif`** re-encodes AVIF images you already serve.

All four are opt-in for the images the module rewrites in your HTML. They sit outside `rewrite_images` and outside the CoreFilters set, so enabling `rewrite_images` alone produces no rewritten AVIF URLs — you have to name the AVIF filters you want.

**Direct image requests are different when the optimizer worker is connected.** When a browser requests the original image URL (in-place optimization) and the module is configured with the [optimizer worker](/docs/worker-configuration/), the worker chooses the format from the request's `Accept` header and needs no AVIF filter: a client that names `image/avif` gets AVIF; any other client that accepts WebP (by naming `image/webp`, `image/*` or `*/*`, or by sending no `Accept`) gets WebP; and when the copy in the negotiated format is not smaller or fails the quality check, the optimized original is served. A client that names `image/avif` never gets WebP instead. A current Chrome therefore typically receives AVIF for original image URLs and WebP for the rewritten URLs in the page. See [Image format and the Accept header](/docs/cdn-integration/#image-format-negotiation).

AVIF is served only when the request advertises `Accept: image/avif`. There is no User-Agent sniffing. A client that does not advertise AVIF falls through the existing format order unchanged. The cache key folds in the client's AVIF capability, so variants are stored and served per capability exactly as WebP variants are.

The AV1 encoder is statically linked into the module. There is no separate package or codec library to install.

### Caveat: `RewriteLevel AllFilters` enables all four

"Opt-in" holds for filters you name explicitly. The four AVIF filters are not in the dangerous-filter set, so `RewriteLevel AllFilters` switches all of them on. If you run `AllFilters`, you are running the AVIF filters whether or not you listed them.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters convert_jpeg_to_avif
ModPagespeedEnableFilters convert_to_avif_lossless
ModPagespeedEnableFilters convert_to_avif_animated
ModPagespeedEnableFilters recompress_avif
```

**nginx:**

```nginx
pagespeed EnableFilters convert_jpeg_to_avif;
pagespeed EnableFilters convert_to_avif_lossless;
pagespeed EnableFilters convert_to_avif_animated;
pagespeed EnableFilters recompress_avif;
```

On IIS, use the same filter names in `pagespeed.config` without the trailing semicolon — see [IIS configuration](/docs/iis-configuration/).

### Configuration

| Parameter                                 | Default | Description                                                                        |
| ----------------------------------------- | ------- | ---------------------------------------------------------------------------------- |
| `AvifRecompressionQuality`                | 60      | AVIF quality level; -1 uses `ImageRecompressionQuality`                            |
| `AvifRecompressionQualityForSmallScreens` | 50      | AVIF quality for small-screen clients; -1 uses `AvifRecompressionQuality`          |
| `AvifAnimatedRecompressionQuality`        | 50      | Quality for animated AVIF output                                                   |
| `AvifQualityForSaveData`                  | 45      | AVIF quality for clients sending `Save-Data: on`                                   |
| `AvifTimeoutMs`                           | 5000    | Wall-clock budget for one AVIF encode; the encode is abandoned when it is exceeded |

These mirror the `Webp*` quality parameters: a value of -1 falls back to the more general
setting, so you can set `ImageRecompressionQuality` alone and let AVIF follow it.

`AvifTimeoutMs` exists because AV1 still-image encoding is materially slower than WebP. The
budget stops one expensive image from occupying a rewrite slot indefinitely; when it is hit
the AVIF candidate is abandoned and the existing WebP or optimized-original output is served,
so a timeout costs CPU but never a broken image. Raise it if large photographic sources are
being skipped, lower it if AVIF encodes are crowding out other rewrites.

**Apache:**

```apache
ModPagespeedAvifRecompressionQuality 60
ModPagespeedAvifTimeoutMs 5000
```

**nginx:**

```nginx
pagespeed AvifRecompressionQuality 60;
pagespeed AvifTimeoutMs 5000;
```

### Risks

- **An AVIF candidate costs a full extra decode and encode per image.** The module encodes an AVIF candidate and keeps it only if it beats the WebP or optimized-original candidate. Each candidate is produced from its own decode; the decode is not shared between formats. AV1 encoding is also slower than WebP encoding at comparable quality, so enabling AVIF measurably raises the CPU cost of the first rewrite of every image. Watch `ImageMaxRewritesAtOnce` and the rewrite deadline on busy origins.
- **Pick-smaller means some of that work is discarded.** When the AVIF candidate is not smaller, it is dropped and the existing variant is served. The CPU is spent either way.
- **Images with C2PA content credentials are not converted.** A source carrying a C2PA manifest is left alone by all four filters, so its provenance chain survives the pipeline.
- **Delivery depends on the `Accept` header reaching the module.** As with `convert_jpeg_to_webp`, a CDN or proxy that rewrites `Accept` or ignores `Vary: Accept` can cross-serve formats. Test with your CDN configuration before rolling out.

## Image stripping filters {#strip_metadata}

<a id="strip_image_color_profile"></a><a id="strip_image_meta_data"></a><a id="jpeg_subsampling"></a>

Full guides: [`strip_image_color_profile`](/docs/filters/strip_image_color_profile/), [`strip_image_meta_data`](/docs/filters/strip_image_meta_data/), [`jpeg_subsampling`](/docs/filters/jpeg_subsampling/)

### What they do

- **`strip_image_color_profile`** removes embedded ICC color profiles from images. Most web browsers ignore ICC profiles, and they can add tens of kilobytes to a file.
- **`strip_image_meta_data`** removes EXIF, XMP, and other metadata from images. This includes camera information, GPS coordinates, thumbnails, and editing history.
- **`jpeg_subsampling`** downsamples JPEG chroma channels (4:2:0 subsampling), reducing file size with minimal visual impact on photographic content.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters strip_image_color_profile
ModPagespeedEnableFilters strip_image_meta_data
ModPagespeedEnableFilters jpeg_subsampling
```

**nginx:**

```nginx
pagespeed EnableFilters strip_image_color_profile;
pagespeed EnableFilters strip_image_meta_data;
pagespeed EnableFilters jpeg_subsampling;
```

### Risks

- Stripping color profiles can cause subtle color shifts on wide-gamut displays or for images authored in non-sRGB color spaces. This affects a small fraction of web images.
- Stripping metadata removes copyright and attribution information embedded in the file. The metadata is removed from the served variant only; original files on disk are not modified.

## Resizing filters {#resize_images}

<a id="resize_rendered_image_dimensions"></a><a id="insert_image_dimensions"></a><a id="insert_img_dimensions"></a>

[Full guide →](/docs/filters/resize_images/) · Also: [`resize_rendered_image_dimensions`](/docs/filters/resize_rendered_image_dimensions/), [`insert_image_dimensions`](/docs/filters/insert_image_dimensions/)

### What they do

- **`resize_images`** resizes images on the server to match the `width` and `height` attributes declared in the `<img>` tag. If an image is 2000x1500 but displayed at 400x300, mod_pagespeed serves a 400x300 variant.
- **`resize_rendered_image_dimensions`** injects JavaScript that reports each image's actual rendered dimensions on the client. On subsequent requests, mod_pagespeed resizes to the rendered size. Requires two page loads to take effect.
- **`insert_image_dimensions`** adds explicit `width` and `height` attributes to `<img>` tags that lack them. This prevents layout shifts (CLS) but does not resize the image file itself. `insert_img_dimensions` is an accepted alternate spelling of the same filter.

`insert_image_dimensions` declares the intrinsic size of each image: it adds `width` and `height` attributes carrying the pixel dimensions of the image file itself, read from the copy the module decoded, to `<img>` and image `<input>` elements that declare no dimensions at all: no `width` attribute, no `height` attribute, and no dimension inside a `style` attribute. An element that already declares any of those is left alone. The browser uses the pair to reserve the image's box before the file arrives, which is what removes the layout shift; the filter does not resize the image file and does not touch CSS sizing. It applies even to images the module decides not to optimize, as long as the file's dimensions could be decoded. It does not apply when image URLs are preserved (`ImagePreserveURLs`, as under OptimizeForBandwidth). Images that `inline_images` turns into `data:` URIs get no dimensions from this filter; `inline_images` instead removes `width` and `height` attributes that match the image's own size. Not a core filter; enable it by name.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters resize_images
ModPagespeedEnableFilters resize_rendered_image_dimensions
ModPagespeedEnableFilters insert_image_dimensions
```

**nginx:**

```nginx
pagespeed EnableFilters resize_images;
pagespeed EnableFilters resize_rendered_image_dimensions;
pagespeed EnableFilters insert_image_dimensions;
```

### Configuration

| Parameter                     | Default | Description                                                               |
| ----------------------------- | ------- | ------------------------------------------------------------------------- |
| `ImageLimitResizeAreaPercent` | 100     | Only resize if the result is this percentage of the original area or less |

### Risks

- `resize_images` only works when `width` and `height` are present on the `<img>` tag. Images sized purely by CSS are not resized.
- `resize_rendered_image_dimensions` injects JavaScript and requires two page loads. The first load serves the original image.
- `insert_image_dimensions` can break responsive layouts that rely on the absence of explicit dimensions. Test with your CSS.

## Inline and preview filters {#inline_images}

<a id="inline_preview_images"></a><a id="dedup_inlined_images"></a><a id="resize_mobile_images"></a>

[Full guide →](/docs/filters/inline_images/) · Also: [`inline_preview_images`](/docs/filters/inline_preview_images/), [`dedup_inlined_images`](/docs/filters/dedup_inlined_images/), [`resize_mobile_images`](/docs/filters/resize_mobile_images/)

### What they do

- **`inline_images`** replaces small image references with `data:` URIs, eliminating the HTTP request. Only images below `ImageInlineMaxBytes` are inlined.
- **`inline_preview_images`** replaces full-size images with a low-quality inline placeholder that loads instantly, then swaps in the full image via JavaScript.
- **`dedup_inlined_images`** replaces repeated inline `data:` URIs on the same page with JavaScript references to the first occurrence, reducing HTML size.
- **`resize_mobile_images`** serves smaller images to mobile devices based on the User-Agent header. Enabling it also enables `inline_preview_images`, which it depends on.

`inline_preview_images` serves a low-quality preview first and the real image after. For each image the critical-images beacon has marked as above the fold, whose optimized size is between `MinImageSizeLowResolutionBytes` (default 3072 bytes) and `MaxImageSizeLowResolutionBytes` (default 1 MB), the optimizer encodes a tiny low-resolution version at quality 10, with profile, metadata and provenance stripped from the throwaway preview. The original `src` (and `srcset`, when present) is renamed to `data-pagespeed-high-res-src` (and `-srcset`). The preview is used only when it is small: it must fit `MaxLowResImageSizeBytes` (default: no cap) and be smaller than the full image under `MaxLowResToFullResImageSizePercentage` (default 100). On desktop the preview becomes the `src` and an `onload` handler on the image swaps in the full image; on mobile with aggressive rewriters on, the previews are injected as scripts after the last previewed image in the flush window, and a script at the end of the body swaps in the full images, on scroll when `LazyloadHighresImages` is enabled. The filter is beacon-driven: until the critical-images finder has data it does nothing, so a site's first views are unchanged. It stands down when the page's Content-Security-Policy forbids the inline scripts the swap relies on.

`dedup_inlined_images` shrinks pages that repeat the same inlined image. When two or more references have become identical `data:` URIs, the first occurrence keeps its bytes and gets an `id`; every later occurrence of the same data URI loses its `src` and instead carries a one-line inline script that copies the first image's `src` back onto the element at run time. There is a floor below which dedup does not pay: a data URI must be longer than 185 bytes, roughly the size of the restoring snippet, or it is left as it is. The filter needs JavaScript, so it is disabled for user agents that cannot lazy-load images and for XMLHttpRequests, and it stands down inside `<noscript>` or when the page's Content-Security-Policy forbids inline scripts. The `num_dedup_inlined_images_candidates_found` and `num_dedup_inlined_images_candidates_replaced` statistics in the admin console show how much it found and how much it replaced.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters inline_images
ModPagespeedEnableFilters inline_preview_images
ModPagespeedEnableFilters dedup_inlined_images
ModPagespeedEnableFilters resize_mobile_images
```

**nginx:**

```nginx
pagespeed EnableFilters inline_images;
pagespeed EnableFilters inline_preview_images;
pagespeed EnableFilters dedup_inlined_images;
pagespeed EnableFilters resize_mobile_images;
```

### Configuration

| Parameter             | Default | Description                                          |
| --------------------- | ------- | ---------------------------------------------------- |
| `ImageInlineMaxBytes` | 3072    | Maximum image size in bytes to inline as a data: URI |

**Apache:**

```apache
ModPagespeedImageInlineMaxBytes 4096
```

**nginx:**

```nginx
pagespeed ImageInlineMaxBytes 4096;
```

### Risks

- Inlining increases HTML size. Inlined images are not cached separately by the browser. Set `ImageInlineMaxBytes` conservatively.
- `inline_preview_images` adds JavaScript and a visible quality transition. Users see a blurry image before the full-resolution variant loads.
- `resize_mobile_images` relies on User-Agent detection. Incorrect UA classification can serve wrong-sized images. It also pulls in `inline_preview_images`, so expect that filter's placeholder-then-swap behavior when enabling it.

## Lazy loading: lazyload_images {#lazyload_images}

[Full guide →](/docs/filters/lazyload_images/)

### What it does

`lazyload_images` defers loading of images that are below the fold, reducing initial page weight and request count.

In v1.15.0+r18 and later, on browsers that support it (the large majority), the filter adds the native `loading="lazy"` and `decoding="async"` attributes and leaves image URLs untouched. Images known to be above the fold are not deferred and instead get `fetchpriority="high"`. On older browsers — and in v1.15.0+r17 and earlier — the filter uses a JavaScript loader that loads images as the user scrolls them into view.

The `LazyloadImagesMode` directive controls this behavior: `auto` (the default) selects native or JavaScript per browser, `native` always uses the attributes, and `js` always uses the JavaScript loader (the behavior of v1.15.0+r17 and earlier).

When the filter has no data yet about which images are above the fold, it leaves the first images of the page alone so the largest visible image is not deferred. `LazyloadImagesSkipFirst` sets how many (default 1).

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters lazyload_images
```

**nginx:**

```nginx
pagespeed EnableFilters lazyload_images;
```

### Configuration

Select the mechanism and tune the above-the-fold protection:

**Apache:**

```apache
ModPagespeedLazyloadImagesMode auto
ModPagespeedLazyloadImagesSkipFirst 1
```

**nginx:**

```nginx
pagespeed LazyloadImagesMode auto;
pagespeed LazyloadImagesSkipFirst 1;
```

To disable lazy loading for a specific image, add the [`data-pagespeed-no-defer`](/pagespeed-markers/#data-pagespeed-no-defer) attribute:

```html
<img src="hero.jpg" data-pagespeed-no-defer />
```

Images that already carry a `loading` attribute are always left untouched, so hand-tuned markup keeps working.

### Risks

- In `js` mode (or `auto` on older browsers), JavaScript-based lazy loading can interfere with image-dependent layout calculations, print rendering, and automated testing tools. The JavaScript mechanism also stands down on pages whose `Content-Security-Policy` disallows inline scripts; native mode is unaffected by CSP.
- Images that are immediately visible (above the fold) should not be lazy loaded. Above-fold detection is data-driven and imperfect; `LazyloadImagesSkipFirst` bounds the damage when no data is available, but pages with unusual layouts may still see a deferred visible image. Use `data-pagespeed-no-defer` on known-critical images.

## Responsive images {#responsive_images}

[Full guide →](/docs/filters/responsive_images/)

### What it does

`responsive_images` adds a `srcset` to `<img>` tags so a high-density screen fetches a sharper file and a 1x screen stops paying for pixels it cannot show. The module generates a resized variant for each configured pixel density, plus the full-sized original, and offers them as `url N.x` candidates; the browser picks by viewport and device pixel ratio. Not a core filter; enable it by name. Live demo: [responsive_images](/examples/responsive_images/).

```html
<!-- before -->
<img src="/photos/team.jpg" width="400" height="300" />

<!-- after: the source file is 1600x1200, so the full-size candidate is 4x -->
<img
  src="/photos/400x300xteam.jpg.pagespeed.ic.HASH.jpg"
  width="400"
  height="300"
  srcset="/photos/600x450xteam.jpg.pagespeed.ic.HASH.jpg 1.5x,/photos/800x600xteam.jpg.pagespeed.ic.HASH.jpg 2x,/photos/1200x900xteam.jpg.pagespeed.ic.HASH.jpg 3x,/photos/xteam.jpg.pagespeed.ic.HASH.jpg 4x"
/>
```

### When it helps and when it does not

It helps where the same page serves both ordinary screens and phones or high-density displays: the 1x visitor downloads the small file instead of the oversized original, and the 2x visitor gets a file that is actually sharp. It adds nothing for an image whose declared dimensions already equal the source file's size: every candidate then resolves to the same file and no srcset is emitted. Every variant is a resized copy in the cache, so a page with many large images multiplies its storage; the `responsive_images_zoom` companion adds a script and refetches on zoom.

### How it decides

The filter needs `src`, `width` and `height` on the `<img>`; an image without declared dimensions, an image that already carries a `srcset`, one marked `data-pagespeed-no-transform`, and 1x1 tracking pixels are all left alone. Densities come from `ResponsiveImageDensities` (default `1.5,2,3`, each must be above zero). Each candidate is resized to the declared dimensions scaled by its density, and the full-sized original joins the list at the resolution its width actually represents. Candidates whose URL or final dimensions equal the previous candidate's are dropped, so an image whose source file is only slightly larger than the 1x size gets a shorter list. If the highest-density variant turns out small enough to inline as a `data:` URI, it becomes the single `src` and no `srcset` is emitted. The filter is wired up only when `resize_images` is also enabled, which it is under CoreFilters through `rewrite_images`.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters responsive_images
```

**nginx:**

```nginx
pagespeed EnableFilters responsive_images;
```

### Configuration

To control which pixel densities are generated (default `1.5,2,3`), use `ResponsiveImageDensities` with a comma-separated list of numbers:

**Apache:**

```apache
ModPagespeedResponsiveImageDensities 1.5,2,3
```

**nginx:**

```nginx
pagespeed ResponsiveImageDensities 1.5,2,3;
```

### Risks

- Generating multiple variants per image increases storage and cache requirements on the server.
- If images have many density variants, the total bytes served across all variants can exceed the original single image. Monitor cache size and bandwidth.
- Requires `width` and `height` attributes on `<img>` tags to calculate variant dimensions.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-responsive_images` comparison; [Is it working?](/docs/is-it-working/) has the steps.

## Sprite images {#sprite_images}

[Full guide →](/docs/filters/sprite_images/)

### What it does

`sprite_images` merges small CSS background images into one sprite sheet and rewrites the stylesheet so each rule shows its own region: the `background-image` URL becomes the sheet at a `.pagespeed.is.` URL, and the `background-position` shifts to the element's slice. A toolbar of icons that cost a request each then arrives in one fetch. Not a CoreFilter; enable it by name. Live demo: [sprite_images](/examples/sprite_images/).

```css
/* before */
.icon-cart {
  background: url(/img/cart.png) no-repeat;
  width: 16px;
  height: 16px;
}
.icon-user {
  background: url(/img/user.png) no-repeat;
  width: 16px;
  height: 16px;
}

/* after: the images are stacked vertically in one sheet */
.icon-cart {
  background: url(/img/cart.png+user.png.pagespeed.is.HASH.png) no-repeat;
  width: 16px;
  height: 16px;
  background-position: 0 0;
}
.icon-user {
  background: url(/img/cart.png+user.png.pagespeed.is.HASH.png) no-repeat;
  width: 16px;
  height: 16px;
  background-position: 0 -16px;
}
```

### When it helps and when it does not

Spriting is an HTTP/1.1 technique: it exists because browsers used to open few connections per host and every icon queued. With HTTP/2 multiplexing the latency benefit is minimal, and the costs remain. Changing one icon invalidates the whole sheet, and a page that shows two icons downloads every icon packed beside them. It can still pay on icon-heavy pages with a stable icon set and a large HTTP/1.1 audience; elsewhere, measure before keeping it.

### How it decides

Only images referenced from a CSS `background` or `background-image` declaration are candidates; `<img>` tags never join a sprite. Only PNG or GIF backgrounds in rules that declare both `width` and `height` are sprited, and GIFs are converted to PNG in the sheet. The module must be able to fetch the image and learn its dimensions, and a declaration the CSS parser cannot understand is left alone rather than guessed at. Positions in the rewritten rules are computed from the packed layout: an existing `background-position` is shifted to the slice, and a rule without one gets a new `background-position` declaration.

### Risks

- Spriting increases cache invalidation scope: changing one image invalidates the entire sprite.
- Only CSS `background-image` references are sprited. Inline `<img>` tags are not affected.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-sprite_images` comparison; [Is it working?](/docs/is-it-working/) has the steps.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters sprite_images
```

**nginx:**

```nginx
pagespeed EnableFilters sprite_images;
```

## In-place browser optimization {#in_place_optimize_for_browser}

[Full guide →](/docs/filters/in_place_optimize_for_browser/)

### What it does

`in_place_optimize_for_browser` is retired. It used to make the module's own in-place path vary images by the browser's `Accept` header and add `Vary: Accept`; that path now optimizes an image identically for every client, so the filter has nothing left to do. The name is still accepted, with a warning, so an existing configuration keeps loading; it is no longer part of `OptimizeForBandwidth`. Remove it from your configuration.

The filter does not control format negotiation at the original URL. When the optimizer worker is attached (`DaemonSocketPath` and `DaemonVolumePath`, which the Apache packages built to depend on `pagespeed-optimizer` set in `pagespeed_daemon.conf`), in-place images are served from the optimizer's cache in the format the request's `Accept` header selects, with `Vary: Accept`: WebP for `*/*`, `image/*` or no `Accept` header, the original format for a request that names only that format. See [Image format and the Accept header](/docs/cdn-integration/#image-format-negotiation) for the full rule and how to get the original format for a non-browser client.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters in_place_optimize_for_browser
```

**nginx:**

```nginx
pagespeed EnableFilters in_place_optimize_for_browser;
```

### Risks

- None from the filter itself: it has no effect. The `Vary: Accept` considerations for images served from the worker's cache are on the [CDN integration page](/docs/cdn-integration/#image-format-negotiation).

## Cache extension for images: extend_cache_images {#extend_cache_images}

[Full guide →](/docs/filters/extend_cache_images/)

### What it does

Core filter, one of the three members of [`extend_cache`](/docs/cache-control/#extend_cache). Rewrites image URLs to content-hashed `.pagespeed.ce.` URLs served with a one-year `Cache-Control` max-age, so browsers keep images for a year while a changed image gets a new URL. Use it when the origin cannot set long cache lifetimes itself; under CoreFilters it is already on through `extend_cache`, and `rewrite_images` produces content-hashed URLs of its own for every image it rewrites. Disabling it leaves `extend_cache_css` and `extend_cache_scripts` on. Live demo: [extend_cache](/examples/extend_cache/).

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters extend_cache_images
```

**nginx:**

```nginx
pagespeed EnableFilters extend_cache_images;
```

## Responsive images zoom {#responsive_images_zoom}

[Full guide →](/docs/filters/responsive_images_zoom/)

### What it does

`responsive_images_zoom` adds a small script next to the `srcset` attributes that [`responsive_images`](#responsive_images) generates, so that when the visitor zooms the page the browser picks a variant that stays sharp at the zoomed size instead of upscaling the one chosen for the original zoom level. Enable it together with `responsive_images`; on its own it does nothing. Not a core filter; test it with your image markup first.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters responsive_images,responsive_images_zoom
```

**nginx:**

```nginx
pagespeed EnableFilters responsive_images,responsive_images_zoom;
```

### Risks

- Adds a script to every page with responsive images.
- Extra variants are fetched on zoom, which costs bandwidth on pages visitors zoom often.

## Experimental: experiment_collect_mob_image_info {#experiment_collect_mob_image_info}

[Full guide →](/docs/filters/experiment_collect_mob_image_info/)

Collects image information for the retired page-mobilization experiment. In the dangerous set, which `RewriteLevel AllFilters` never enables; not for production.

```nginx
pagespeed EnableFilters experiment_collect_mob_image_info;
```

## Prioritize critical images {#prioritize_critical_images}

[Full guide →](/docs/filters/prioritize_critical_images/)

### What it does

`prioritize_critical_images` sets `fetchpriority="high"` on the images the critical-images beacon has reported above the fold, so the browser front-loads the fetches that most influence Largest Contentful Paint. It is beacon-driven: the page is instrumented, real browsers report which images render above the fold, and the attribute is applied on subsequent responses. It rewrites attributes only and injects no scripts. Enabling the filter also turns on critical-images beaconing.

The filter is a strict no-op until beacon data is available — with no data it would have to guess, and a wrong guess would prioritize a below-the-fold image at the real LCP image's expense. An author-supplied `fetchpriority` always wins, so hand-tuned markup is left untouched.

This filter is opt-in and is not part of any rewrite level. Enable it by name.

### Directives

**Apache:**

```apache
ModPagespeedEnableFilters prioritize_critical_images
```

**nginx:**

```nginx
pagespeed EnableFilters prioritize_critical_images;
```

### Risks

- Above-fold detection is data-driven and imperfect. The strict no-op-without-data behavior avoids the worst case, but on unusual layouts the beacon may still flag an image that is not truly the largest one.
- The filter backs off on `Save-Data` requests, AMP documents, and disallowed URLs.

## Tuning parameters

| Parameter                          | Default  | Description                                                                 |
| ---------------------------------- | -------- | --------------------------------------------------------------------------- |
| `ImageRecompressionQuality`        | 85       | General quality for recompressed images (-1 to 100; -1 uses source quality) |
| `JpegRecompressionQuality`         | -1       | JPEG-specific quality; -1 uses `ImageRecompressionQuality`                  |
| `WebpRecompressionQuality`         | 80       | WebP quality level                                                          |
| `WebpAnimatedRecompressionQuality` | 70       | Animated WebP quality level                                                 |
| `ImageInlineMaxBytes`              | 3072     | Maximum image size in bytes to inline as a data: URI                        |
| `ImageLimitOptimizedPercent`       | 100      | Only serve the optimized image if it is smaller by this percentage          |
| `ImageLimitResizeAreaPercent`      | 100      | Limit on resize area relative to original                                   |
| `ImageResolutionLimitBytes`        | 33554432 | Maximum decoded image size in bytes (4 bytes per pixel); larger images are skipped |
| `ImageMaxRewritesAtOnce`           | 8        | Server-wide limit on parallel image optimizations                           |

**Apache:**

```apache
ModPagespeedImageRecompressionQuality 85
ModPagespeedJpegRecompressionQuality 75
ModPagespeedWebpRecompressionQuality 80
ModPagespeedWebpAnimatedRecompressionQuality 70
ModPagespeedImageInlineMaxBytes 3072
ModPagespeedImageLimitOptimizedPercent 100
ModPagespeedImageLimitResizeAreaPercent 100
ModPagespeedImageResolutionLimitBytes 33554432
ModPagespeedImageMaxRewritesAtOnce 8
```

**nginx:**

```nginx
pagespeed ImageRecompressionQuality 85;
pagespeed JpegRecompressionQuality 75;
pagespeed WebpRecompressionQuality 80;
pagespeed WebpAnimatedRecompressionQuality 70;
pagespeed ImageInlineMaxBytes 3072;
pagespeed ImageLimitOptimizedPercent 100;
pagespeed ImageLimitResizeAreaPercent 100;
pagespeed ImageResolutionLimitBytes 33554432;
pagespeed ImageMaxRewritesAtOnce 8;
```

### Large images and `ImageResolutionLimitBytes`

`ImageResolutionLimitBytes` bounds how large an image may be once decoded, assuming 4 bytes per pixel. The default of 33554432 (32 MiB) therefore allows about 8.39 megapixels (8,388,608 pixels). Images above that, such as 12 MP camera originals at 4000x3000, are passed through untouched: the original is served unchanged, with no error, and each skip is counted in the `image_norewrites_high_resolution` statistic. Check that statistic if large photos are not being optimized. Even a 4000x2250 image (9 MP) is over the limit, while a 2000x1500 image (3 MP) is well under it.

The limit exists to bound memory use. To opt in to larger images, raise it to 64 MiB (about 16.8 MP, enough for 4000x3000 and 4000x4000):

**Apache:**

```apache
ModPagespeedImageResolutionLimitBytes 67108864
```

**nginx:**

```nginx
pagespeed ImageResolutionLimitBytes 67108864;
```

Decoding happens for each image being rewritten, so peak memory grows with the limit multiplied by the number of concurrent rewrites (see `ImageMaxRewritesAtOnce`). At 64 MiB, a single image may need roughly 64 MiB of decoded pixel data, and eight concurrent rewrites can reach several hundred MiB, before resize and encode copies. Raise it only if the server has the headroom. Resizing large originals before upload is another way to stay under the limit.

## Design background: how server-side image rewriting works

:::note[Historical context]
This summarizes a design decision from the original mod_pagespeed project (Google, 2010), included as background. The filter behavior documented above is the current reference for mod_pagespeed 2.
:::

The image filters above descend from a 2010 design for server-side image rewriting. Four invariants from that design still shape how `rewrite_images` works:

1. **Fetch and cache the original bytes asynchronously.** A rewrite never blocks the response. On the first request the original image is served and the optimization runs in the background (bounded by `RewriteDeadlinePerFlushMs`); the optimized variant is served from cache on subsequent requests.
2. **Compare displayed dimensions to natural dimensions.** When the page uses an image smaller than the source, `resize_images` rescales it to the size actually rendered instead of shipping full-resolution pixels the browser will only shrink.
3. **Recompress per format, and only keep a smaller result.** Each format has its own codec strategy (`recompress_jpeg`, `recompress_png`, `recompress_webp`). A conversion (`convert_jpeg_to_webp`, `convert_png_to_jpeg`, `convert_gif_to_png`) or recompression is kept only when the output is smaller, which is what `ImageLimitOptimizedPercent` enforces.
4. **Cache the negative result too.** If no variant is meaningfully smaller, the original URL is kept, and mod_pagespeed remembers that so it does not re-attempt a losing rewrite on every request.

The output formats have changed since 2010: the native module transcodes to WebP through `rewrite_images`, and to AVIF through [opt-in filters](#avif) enabled separately from it (direct requests for the original image URL are negotiated by the optimizer worker and need no AVIF filter, see [AVIF filters](#avif)). The mod_pagespeed optimizer worker adds SVG auto-vectorization and Jpegli through its own independent optimization engine. The four invariants above describe how the native module's `rewrite_images` pipeline works. For how this works in production, see [Automatic WebP/AVIF on nginx: One Decode, 37 Variants](/blog/viewport-aware-image-optimization/) and [Image optimization cost: self-hosted vs CDN](/blog/economics-of-image-optimization/).

_Adapted from the original mod_pagespeed image-rewriting design (Google, 2010), an open-source project now maintained by We-Amp B.V. Original material © Google Inc., released under the Apache License 2.0. mod_pagespeed and PageSpeed are trademarks of Google LLC. We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently._

## See also

- [Filter selection](/docs/filter-selection/) -- how to enable and disable filters
- [PageSpeed filters](/docs/filters/) -- all filters at a glance
- [How the metadata cache works](/how-it-works/metadata-cache/) -- how an image is optimized once and served from cache thereafter

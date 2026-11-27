---
title: 'nginx image optimization module: automatic WebP, opt-in AVIF'
description: 'A native nginx module that transcodes images to WebP and AVIF on serve, negotiates on Accept and caches the variant. Signed apt/yum packages, no compiling.'
date: 2026-06-06
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['nginx', 'image-optimization', 'webp', 'avif']
product: '1.1'
draft: false
---

If you want nginx itself to convert images to WebP or AVIF, your options are narrow. You can write a transcoding pipeline yourself with libvips or ImageMagick, you can compile [ngx_pagespeed](/alternatives/ngx-pagespeed/) against your nginx, or you can install a packaged module. This post is about the last option: a native nginx module that does the conversion in-process, picks the right format per request, and caches what it produces.

The mod_pagespeed module is that module. It runs inside nginx (and Apache) as a native module, with no sidecar or separate service in the request path. It installs from a signed apt or yum repository. The rest of this post covers what a server-layer image module actually does, how to install it, how to switch on AVIF, and where it fits against the do-it-yourself alternative: a libvips or ImageMagick script you wire in yourself.

> **Update — 2026-07-19:** This post originally sent readers to ModPageSpeed 2.0 for AVIF, because the 1.15 module only transcoded WebP. mod_pagespeed 1.15 now encodes AVIF too, on nginx as well as Apache and the native IIS module, with the AV1 encoder linked into the module — there is no extra package to install. The two formats reach you differently and the post has been revised to say so: WebP conversion is part of the default rewriting, while AVIF is enabled by turning on its filters. The sections below reflect the current module; the 2.0 comparisons have been rescoped to what is still 2.0-only.

> **Correction — 2026-11-15:** The statement below that AVIF is something you turn on holds for the image URLs the module rewrites in your pages. It does not hold for direct requests to the original image URL when the optimizer worker is connected: the worker negotiates the format from the browser's `Accept` header, and a browser that names `image/avif` (current Chrome does) gets AVIF with no AVIF filter enabled, provided it is smaller and passes the quality check. See [AVIF filters](/docs/image-filters/#avif).

## What a server-layer image module does

Serving a smaller image involves more than the encode step. A module that lives inside nginx handles four things on your behalf.

### Transcode on serve

When a browser requests `/images/hero.jpg`, the module decodes the original and re-encodes it to WebP. A JPEG becomes a smaller WebP; a PNG with a flat palette becomes a much smaller WebP. WebP beats the original JPEG or PNG substantially at the same visual quality. The output is generated from your existing image URLs, so nothing in your templates or CMS changes.

[AVIF](/blog/avif-vs-webp-2026/) is the same story one format further on, and it compresses smaller still. The module encodes it from the same source, through the same URLs. The difference is how you get it: WebP conversion runs as part of the default rewriting, while AVIF is something you turn on. [Turning on AVIF](#turning-on-avif) below is the one-line version.

### Encode candidates, keep the smallest

For a single image the module can produce more than one candidate: a WebP, an AVIF if you have enabled it, and an optimized version of the original format. It compares the results and keeps the smallest. AVIF generally wins on photographic JPEGs; where it does not, the smaller WebP or optimized original is kept — so switching AVIF on cannot make an image larger.

That comparison is not free. Each candidate is encoded from the source independently, so enabling AVIF adds an encode pass per image on top of the WebP one, and AV1 encoding is slower than WebP encoding at comparable quality. The cache described below is what makes this affordable: the work happens once per variant, not once per request.

### Cache the variant

Each generated variant is written to a disk cache keyed by the source URL and the client capabilities it was built for. The next visitor whose browser matches that capability gets a cache hit — the module serves the stored file directly, with no re-encoding. Transcoding happens once per variant, not once per request. For how the mod_pagespeed optimizer worker extends the same idea to a much wider variant matrix — viewport class, pixel density, and Save-Data on top of format — see [Automatic WebP/AVIF on nginx: one decode, 37 variants](/blog/viewport-aware-image-optimization/).

### Content-negotiate on Accept

Browsers announce what they can decode in the `Accept` header. A request carrying `image/webp` gets WebP; an older client that announces no modern format gets an optimized variant of the original. WebP support is effectively universal across current browsers, so nearly every visitor gets the smaller file, with automatic fallback for the rare client that cannot take it — all from one set of URLs. AVIF joins the same negotiation once its filters are on: a request that announces `image/avif` gets AVIF — roughly 93–94% of global traffic as of 2026 — and everything else falls back through the same order it already used.

## How to install it

The module ships as a prebuilt, signed package from `packages.modpagespeed.com`. No compiler, no nginx source tree, no matching the module ABI to your nginx build by hand. Debian 12/13 and Ubuntu 22.04/24.04 are covered on both amd64 and arm64; Enterprise Linux 9 (AlmaLinux, Rocky, RHEL) is covered on x86_64 and aarch64, and Enterprise Linux 10 on x86_64.

On Debian or Ubuntu:

```bash
# Add the signed apt repository
curl -fsSL https://packages.modpagespeed.com/pubkey.gpg \
  | sudo gpg --dearmor -o /usr/share/keyrings/modpagespeed.gpg

echo "deb [signed-by=/usr/share/keyrings/modpagespeed.gpg] \
https://packages.modpagespeed.com/apt $(lsb_release -cs) main" \
  | sudo tee /etc/apt/sources.list.d/modpagespeed.list

sudo apt update
sudo apt install nginx-module-pagespeed
```

On Enterprise Linux 9 or 10 (AlmaLinux, Rocky, RHEL):

```bash
sudo tee /etc/yum.repos.d/modpagespeed.repo <<'EOF'
[modpagespeed]
name=ModPageSpeed
baseurl=https://packages.modpagespeed.com/yum/el$releasever/$basearch
enabled=1
gpgcheck=1
gpgkey=https://packages.modpagespeed.com/pubkey.gpg
EOF

sudo dnf install nginx-module-pagespeed
```

The package loads the module itself
(`sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx /usr/share/nginx/modules`
shows its line), so there is no `load_module` line to add. Turn it on in
`nginx.conf`:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/pagespeed;
```

Reload nginx and image requests start coming back as WebP, matched to each client. The full repository setup, GPG details, and the Apache package live on the [apt/yum install page](/download/apt-yum/). The signed package is the direct answer to the problem in [ngx_pagespeed won't build against modern nginx](/blog/ngx-pagespeed-wont-build-modern-nginx/): there is no build to fail.

## Turning on AVIF

WebP conversion is part of the default rewriting. AVIF is not: it sits behind its own filters, so your image output does not change until you enable them. For most sites one line is the whole change:

```nginx
pagespeed EnableFilters convert_jpeg_to_avif;
```

That covers the common case — photographic JPEGs going out as AVIF to clients that announce `image/avif`. Three further filters cover the rest of the surface: `convert_to_avif_lossless` for flat-palette and screenshot-style sources, `convert_to_avif_animated` for animated ones, and `recompress_avif` for AVIF you already serve and want re-encoded.

The AV1 encoder is linked into the module itself. There is no second package to install and no encoder library to source separately — the signed `nginx-module-pagespeed` you installed above already contains it. Everything the earlier sections described — candidate comparison, cached variants, `Accept`-header matching — applies to the AVIF output exactly as it does to WebP.

## Versus a DIY libvips or ImageMagick script

libvips is excellent, and so is ImageMagick. If you wire one of them into a build step or a request handler, you can produce WebP too. The difference is everything around the encoder.

With a DIY pipeline, you own:

- **The cache.** You decide where variants live, when they expire, how they are keyed, and how eviction works when the disk fills.
- **The format logic.** You read the `Accept` header yourself, decide the fallback order, and serve the right file for every image on every route.
- **The size comparison.** A newer format is not automatically a smaller file for a given image. Checking that the WebP or AVIF you just produced actually beat the original, per image, and keeping whichever won is logic you write and maintain yourself.
- **The integration.** A script in a build step optimizes images you know about at build time. Images uploaded to a CMS, generated by users, or referenced by third-party CSS are not in that set unless you extend the pipeline to reach them.

None of this is hard in isolation. It adds up to a small service that you maintain, monitor, and debug. The module folds all four into one package that updates through your existing apt or yum channel. You trade the flexibility of a hand-built pipeline for not having to build, run, or patch one.

## Scope: this optimizes the origin, not the edge

One thing this module is not: a CDN. It optimizes images at your origin server. There are no global points of presence, no edge nodes in other regions, no anycast network shortening the round trip for a visitor on the far side of the world. A CDN-based image service distributes the optimized bytes close to the user. This module distributes nothing. It makes the bytes smaller where they already live.

For a lot of sites that is the right trade: the images get much smaller, your content never leaves your infrastructure — [self-hosted image optimization](/self-hosted-image-optimization/) in the literal sense — and the cost is flat instead of metered per transformation. If global edge latency is your primary constraint, the two approaches compose: run the module at the origin to shrink the bytes, and put a CDN in front to distribute them.

The server-layer module gives you automatic WebP and opt-in AVIF, content-negotiated and cached, installed from a signed package with no build step. For SVG auto-vectorization and a wider variant matrix — viewport class, pixel density, and Save-Data alongside format — [mod_pagespeed](/) runs the same lineage as an nginx reverse-proxy worker. See the [product page](/) for the supported servers and the [feature list](/features/) for what else the module rewrites beyond images.

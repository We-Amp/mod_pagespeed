---
title: 'AVIF vs WebP in 2026: which to serve, and how to serve both'
description: 'AVIF beats WebP on size but is slower to encode. Serve AVIF, WebP or the original per request off the Accept header instead of one format for everyone.'
date: 2026-06-06
lastUpdated: 2026-10-22
tags: ['avif', 'webp', 'image-optimization', 'content-negotiation', 'performance']
product: '2.0'
howTo:
  name: How to serve AVIF and WebP per request off the Accept header
  description: Set up per-request image format negotiation so each browser gets AVIF if it accepts it, WebP if not, and the original as a last resort, all from a single source image and decided by the Accept header instead of hand-written markup.
  tools:
  - mod_pagespeed
  - nginx
  steps:
  - name: Run optimization at the server layer
    text: Put the mod_pagespeed module in front of your origin as a reverse proxy. The module reads the request's Accept header and classifies the client's format support before it touches the cache.
  - name: Let the worker produce the variants once
    text: On the first request for an image, the worker decodes the source once and encodes the format variants (AVIF, WebP, and an optimized original), writing each into the shared cache keyed by what the client supports. No build step and no separate asset pipeline.
  - name: Serve the best variant the client accepts
    text: On every subsequent request the module picks the smallest format the Accept header says the browser can decode. AVIF for browsers that send image/avif, WebP for clients that send image/webp, image/* or */* (or no Accept header), the optimized original for a client whose Accept names neither AVIF, WebP nor a wildcard. The cache hit is served via mmap with no re-encode.
  - name: Verify with curl and a real browser
    text: "Send a request with an explicit Accept header (curl -H 'Accept: image/avif,image/webp,*/*') and confirm the Content-Type comes back as image/avif. Drop avif from the header (curl -H 'Accept: image/webp,*/*') and confirm it falls back to image/webp. Then send a header with no image/webp, image/* or */* (curl -H 'Accept: image/jpeg,image/png,image/gif') and confirm you get the original; a wildcard or a missing Accept header gets WebP. Check that the response varies on Accept, then confirm your CDN keeps the formats apart."
---

"AVIF or WebP?" assumes you pick one format and ship it to everyone. You don't have to. Different browsers accept different formats, and the right format for a given image is the smallest one the requesting browser can decode. That is a per-request decision, not a project-wide one.

The questions worth answering are: when is AVIF worth it, when is WebP the safer call, and how do you serve both without hand-maintaining the fallback chain.

## The encode tradeoff: AVIF is smaller, WebP is cheaper to produce

AVIF and WebP both beat JPEG by a wide margin. The difference between the two is narrower, and it cuts in opposite directions on file size and encode time.

At visually-equivalent quality, AVIF files are roughly 20–30% smaller than WebP for typical photographic content. AVIF also supports 10- and 12-bit color depth, wide-gamut and HDR content, and tends to band less on flat regions and gradients. For a hero image or a full-bleed product shot, that extra 20–30% is worth chasing.

The cost is encode time. WebP encodes quickly. AVIF, at comparable effort, is slower: at default encoder settings the gap is roughly an order of magnitude, and high-effort AVIF settings widen it further. For a single image that does not matter. Encoding a 10,000-image catalog synchronously, in the request path with a browser waiting, does. AVIF's compression comes from spending CPU, and that CPU has to be spent somewhere.

This is why the encode happens [outside the request path](/blog/economics-of-image-optimization/) in the mod_pagespeed optimizer worker. The worker decodes the source once and runs the encoders asynchronously. The first visitor gets the original immediately; the AVIF and WebP variants land in the cache shortly after, for everyone who follows. A slow encoder costs nothing when no request waits on it.

## Browser support in 2026

WebP support is effectively universal: around 97% of global traffic, supported in every current browser for years. AVIF is close behind at roughly 93–94% and rising. It ships by default in Chrome (since 85), Edge (since 121), Firefox, Opera, Samsung Internet, and Safari (since 16.4, through the current release). The remaining gap is older Safari versions and the long tail of devices that never update.

Two things follow from those numbers. AVIF no longer needs a feature flag; for most traffic it works. But the remaining 6–7% is real, and it is not evenly distributed. If your audience skews toward older iOS devices or locked-down enterprise fleets, your AVIF-incapable share runs higher than the global average. You cannot ship AVIF to everyone, so you need a fallback. That is why picking a single format does not solve the problem.

## The right answer is content negotiation on the Accept header

Every browser tells you what it can decode, on every image request, in the `Accept` header. Chrome 155 on desktop and Android (October 2026) sends:

```
Accept: image/jxl,image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8
```

A browser without AVIF support omits `image/avif`. One without WebP omits that too. The header is a per-request statement of what this specific client will accept. It is more reliable than user-agent sniffing, and it stays current without you editing a `<picture>` block.

The rule is mechanical:

- `image/avif` present → serve AVIF (smallest).
- else `image/webp`, `image/*` or `*/*` present, or no `Accept` header at all → serve WebP.
- else → serve the optimized original.

The wildcards count: `*/*` and `image/*` say the client takes any image type, and a request without an `Accept` header means the same under RFC 9110. So `curl` with its default `Accept: */*` gets WebP. AVIF needs `image/avif` by name.

One source image, three possible answers, decided at request time from one header.

### Why doing this by hand goes wrong

You can express the fallback chain in markup with `<picture>` and multiple `<source>` elements. It works, and for a handful of curated hero images it is fine. At scale it runs into problems that have nothing to do with the format choice:

- **It only covers the `<img>` tags you control.** CSS `background-image`, images injected by a CMS or a third-party widget, images in email templates or RSS feeds do not go through your `<picture>` markup. They keep serving JPEG.
- **You have to produce, store, and name every variant up front.** That means a build step, an asset pipeline, and a naming convention to maintain. Add a format later and you re-run it across the whole library.
- **It encodes a support assumption into static HTML.** The `<picture>` element does not read the `Accept` header; the browser picks a `<source>` by MIME type from the set you authored. Change a format or a quality target and you are editing markup, not configuration.
- **It fails quietly when it drifts.** A template that drops the AVIF `<source>` does not error. It serves WebP to everyone, and you find out from a bandwidth graph rather than a build failure.

The markup approach puts a person in charge of a decision the server already has enough information to make on every request, for every image, including the ones no template touches.

## How the server layer does it automatically

Move the decision to where the `Accept` header arrives, at the server, and the per-image bookkeeping disappears.

The module classifies each request into a capability mask that includes the formats the client accepts, read off the `Accept` header. It looks up the requested image in the [variant-aware cache](/features/) and serves the best-fit variant for that mask: AVIF to a browser that sent `image/avif`, WebP to one that sent `image/webp`, `image/*` or `*/*` but not `image/avif`, the optimized original to the rest. If the ideal variant is not in the cache yet, the selector degrades to the next-best one rather than failing.

The variants are produced once. On a cache miss the worker reads the source, decodes it a single time, and encodes the formats from that one decode pass, writing each back into the shared cache. A 10-megapixel JPEG decodes to tens of megabytes in memory, so decoding once and fanning out to AVIF, WebP, and an optimized original keeps the CPU cost in check. The slow AVIF encode runs in the worker, off the request path, so no visitor waits on it.

Two properties make this safe to leave running:

- **It covers every image the server serves**, not only the ones in `<picture>` markup, because the decision lives below the HTML at the proxy.
- **Responses vary on `Accept`.** That tells a downstream cache to keep formats apart, but CDNs differ in what they do with it: some don't cache the response, some ignore the header. [Check yours with four curl commands](/blog/jpeg-xl-accept-header-and-your-cdn-image-cache/#four-curl-commands-against-your-own-cdn) before relying on it.

### Verify it with curl and a real browser

Confirm the negotiation from the command line before trusting it in production. Send a request with an explicit `Accept` header and confirm the `Content-Type` comes back as `image/avif`:

```
curl -sI -H 'Accept: image/avif,image/webp,*/*' https://example.com/hero.jpg | grep -i 'content-type\|vary'
# content-type: image/avif
# vary: Accept
```

Drop `avif` from the header and confirm it falls back to `image/webp`, then send `Accept: image/jpeg,image/png,image/gif` (any header without `image/webp`, `image/*` or `*/*`) and confirm you get the optimized original. Keep the wildcard out of that last request: `*/*`, `image/*` or no `Accept` header at all selects WebP, because a wildcard says the client takes any image type. Check that the response varies on `Accept`, then [confirm your CDN keeps the formats apart](/blog/jpeg-xl-accept-header-and-your-cdn-image-cache/#four-curl-commands-against-your-own-cdn). Then load the page in a real browser and read the `Content-Type` of the image request in the network panel. It should match what that browser advertises.

The same content-negotiation machinery drives [viewport-aware resizing](/blog/viewport-aware-image-optimization/): the capability mask carries screen class and pixel density alongside format support, so one source image resolves to the right format *and* the right size for the requesting client. Format is one axis of the same per-request decision.

## So: AVIF or WebP?

Both. AVIF where the browser accepts it, because it is smaller. WebP where it does not, because it is well ahead of JPEG and supported almost everywhere. The original for the last few percent that take neither. The choice only looks like an either/or when you decide for every visitor at once. Decide per request off the `Accept` header instead, and every client gets the smallest format it can render.

The work is in producing the variants once and selecting the right one on every request without hand-maintaining the chain. That is a [self-hosted, server-layer job](/self-hosted-image-optimization/), and the one [mod_pagespeed](/features/) is built to do. It also helps [LCP](/core-web-vitals/lcp/): the largest contentful element is often an image, and serving it as AVIF instead of JPEG is usually a large byte cut on the page.

---

*Browser support figures reflect global usage data as of mid-2026 and shift over time; confirm current numbers against [caniuse.com/avif](https://caniuse.com/avif) before making a hard cutover decision.*

---
title: 'Content-hash URLs: why .pagespeed. URLs carry a hash'
description: 'A content hash in a .pagespeed. URL gives year-long caching with automatic cache busting, years before webpack contenthash. Plus data-pagespeed-url-hash.'
date: 2026-06-05
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['caching', 'architecture', 'performance']
draft: false
---

Watch mod_pagespeed rewrite a page and you will see URLs like this go by:

```
styles.css.pagespeed.ce.GhT8kP2mNq.css
logo.png.pagespeed.ic.7Vx0aLd9Wf.png
app.js.pagespeed.jm.bQ3rZ1cYps.js
```

The interesting part is the string in the middle. It is not a version number or a build timestamp we picked; it is a hash of the file's content. That lets the optimizer put a one-year cache lifetime on every asset and stop worrying about cache invalidation. The idea is old enough that I want to write down where it came from. Front-end build tools arrived at the same answer years later and gave it a name.

## What the content hash actually is

When the [`extend_cache`](/docs/cache-control/#extend_cache) filter rewrites a resource URL, it computes a hash over the bytes of that resource and embeds the hash in the new filename. The hash is a function of the content and nothing else. Two files with identical bytes get the same hash; change a single byte and the hash changes too.

That property is what the rest of this builds on. Because the URL is derived from the content, the URL is a promise: this exact byte sequence, for as long as the URL exists. So mod_pagespeed serves it with `Cache-Control: max-age=31536000` (one year) and lets browsers and CDNs hold onto it as long as they like. There is no risk of serving a stale file. A changed file has different bytes, so it hashes differently, so it lands at a URL the browser has never seen and fetches fresh.

You can cache a content-hashed URL forever and never serve stale bytes, because a given URL only ever names one byte sequence. The usual choice between a [short TTL with frequent revalidation and a long TTL with the occasional stale response](/blog/cache-mode-safety-must-revalidate-vs-aggressive/) does not apply here. There is no expiry clock to get wrong.

A practical consequence: you almost never need to purge these resources. The mod_pagespeed module ships [real cache purging for the cases that need it](/blog/single-url-cache-purge-optimizing-proxy/): the admin endpoint, the HTTP `PURGE` method, and the `cache.flush` file. But an asset under a content-hashed URL purges itself the moment its content changes and the URL changes with it. The old URL stops being referenced. Deploy a new stylesheet and clients fetch the new hash. Nothing has to tell them to discard the old one, because they will never request it again.

One caveat, because people hit it. In-Place Resource Optimization (IPRO) deliberately keeps a resource's original URL, so that third-party code and CDN edge rules that hardcode the path keep working. Those resources do not get a content-hashed URL, so they fall back to whatever `Cache-Control` your origin already sends, or to a [default TTL heuristic](/blog/default-cache-ttl-heuristic-freshness/) when the origin sends none. Content hashing is a property of URL rewriting, not of optimization in general.

## The 2010 cache-busting design call

This was decided early. One of the original mod_pagespeed design documents, *Resource Cache Extension* (April 2010), worked through exactly this: rewrite resource URLs to carry a content-derived hash, so the server can hand out long, aggressive cache lifetimes without ever serving stale bytes, and so a content change busts the cache by construction instead of by a manual step.

At the time this was an unusual thing for a web server to do on its own. Build tools were not doing it for you. The common advice for cache busting was to append a query string (`style.css?v=3`) and bump the number by hand, or to lean on `Last-Modified` and `ETag` revalidation round-trips. Hashing the content and putting the hash in the path was the better answer. An intermediary that knows nothing about your versioning scheme still treats a changed file as a genuinely different resource. mod_pagespeed did it automatically, at the proxy, with no change to your source.

## When content hashing became standard practice

The pattern is now everywhere, under the name fingerprinting or content hashing. Anyone who has shipped a front-end build has already configured it, probably without thinking of it as a caching strategy:

- webpack popularized `[contenthash]` in output filenames (`main.8f3a1c.js`) around 2016, for precisely this reason: cache the bundle for a year, and let a content change produce a new filename.
- Vite, Rollup, and Next.js do the same by default. Hashed asset names are table stakes for a modern bundler.
- The HTTP spec eventually wrote it down. `Cache-Control: immutable` (RFC 8246, 2017; shipped in Firefox 49 in 2016, later in Chromium) tells the browser not to bother revalidating on reload. It is the formal version of what content-hashed URLs had been assuming all along: if the name is the hash, revalidation is pointless.

I am not claiming mod_pagespeed invented content addressing. The idea predates all of us, and Git is the more famous example of naming things by their hash. The narrower claim, and I think a fair one, is this: mod_pagespeed was rewriting live production asset URLs to content hashes and serving them with year-long lifetimes in 2010, several years before that became the default behavior of front-end build tooling and before the platform gave it a header. It treated your assets the way a bundler now treats its output, except it did it at request time, with no build step.

## Reading .pagespeed. URLs and data-pagespeed-url-hash

If you are debugging a site running mod_pagespeed, here is how to read these URLs.

The format is `originalName.pagespeed.FILTER_ID.HASH.extension`. The two-letter `FILTER_ID` tells you which filter produced the resource: `ce` for cache extension, `ic` for image rewriting, `jm` for JavaScript minification, `cc` for combined CSS. The `HASH` is the content signature described above. So `app.js.pagespeed.jm.bQ3rZ1cYps.js` is the JavaScript-minified variant of `app.js`, whose minified bytes hash to `bQ3rZ1cYps`.

What is `data-pagespeed-url-hash`, then? It is a different hash, and the two get confused constantly. If you see a `data-pagespeed-url-hash` attribute on an image tag, that is not the content hash from the URL above. It is a hash of the image's *original URL*, and it exists for the critical-image beacon, the small piece of injected JavaScript that reports which images rendered above the fold. The beacon needs to name an image back to the server without understanding any of the URL rewriting the server did, such as cache-extended URLs, domain mapping, or inlined data URIs. A hash of the original URL gives it a stable, compact identifier to put in the beacon POST. So `data-pagespeed-url-hash` is a hash of the original URL, while the hash inside a `.pagespeed.` URL is a hash of the resource's content.

## Content hashing in the module and the optimizer worker today {#content-hashing-in-mod_pagespeed-115-and-20-today}

Content-hashed cache extension is a CoreFilter in the module (`extend_cache`, on by default). It is cheap to run and hard to get wrong, and it applies to every asset. The approach has not needed to change since 2010.

The optimizer worker took the other path. Because modern build tools now fingerprint assets for you, the worker does not rewrite URLs to content hashes itself. Instead it respects the fingerprinted, `immutable` assets your build already emits, capping their lifetime at `pagespeed_immutable_max_age`, and uses [conditional revalidation](/blog/conditional-revalidation-304-vs-active-purge/) (`ETag` / `If-None-Match`) for everything else. The technique that had to live in the proxy in 2010 now lives in your bundler, so the proxy can defer to it.

If you are running a Google build of mod_pagespeed that is no longer actively developed and want the maintained, actively developed line that still does all of this, that is what we ship. See the [downloads](/download/) or the 2.0 [cache-control behavior](/docs/cache-control/). Under the hood, the [metadata cache remembers each content hash](/how-it-works/metadata-cache/), so a resource is optimized once and served from cache thereafter.

---

*Adapted in part from the original mod_pagespeed design documentation (Google, 2010–2018) — specifically [Resource Cache Extension](https://github.com/we-amp/mod_pagespeed/wiki/Design-Doc:-Resource-Cache-Extension) — an open-source project now maintained by We-Amp B.V. Original material © Google Inc., released under the Apache License 2.0. mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

---
title: 'Template hashing: compute critical CSS once, reuse it'
description: 'How the mod_pagespeed worker hashes a page template so critical CSS is computed once and reused across every URL sharing it, with queue dedup.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['critical-css', 'headless-chrome', 'performance', 'architecture', 'caching']
draft: false
product: '2.0'
---

A store with 100,000 product URLs does not have 100,000 layouts. It has a handful of page templates: a product page, a category listing, a search results page, a blog post, and a homepage. The same `<head>`, the same header, the same grid, the same footer; only the text and the image URLs change from one product to the next. If you compute critical CSS by rendering each URL in a headless browser, you pay for 100,000 renders to learn five answers, one per template. Hashing the template structure is how you stop paying for the other 99,995.

That arithmetic is the whole problem with browser-based critical CSS extraction. A render in `chrome-headless-shell` costs 200-800ms of CPU and 50-100MB of RAM per page. At 100,000 URLs that is hours of compute and a memory bill no one wants to defend. The fix is not a faster browser. The fix is to stop rendering the same template twice.

The optimizer worker groups thousands of URLs into a handful of templates by hashing their DOM structure, so a critical-CSS profile computed once is reused across every URL that matches, and the analysis queue skips URLs whose template already has a profile. That is the cost-amortization mechanism that makes browser-validated critical CSS affordable on a real site.

## Hash the template structure to reuse critical CSS

Two product pages on the same store are nearly identical documents. Same tag hierarchy, same class names, same nesting depth. What differs is the part that does not affect which CSS rules apply: the product title text, the price, the image `src`, the SKU in a `data-` attribute. The cascade does not care that one hero image is `widget-a.jpg` and the next is `widget-b.jpg`. The rules that fire are the same.

The optimizer worker's template detector exploits exactly this. `TemplateDetector::HashStructure` walks the scanned elements and folds each one's nesting depth and tag name into a 64-bit FNV-1a hash (seeded with `kFnvOffsetBasis`), ignoring text content, ids, classes, and attribute values; it also mixes in the number of stylesheet links and whether inline CSS is present, since those are structural features that separate one template from another. Two pages that produce the same hash are treated as the same template. The product detail page for a blue widget and the product detail page for a red widget collapse to one template hash, because the only differences between them live in the data the hash deliberately throws away.

The math falls out immediately:

- 100,000 URLs across 15 templates: 15 browser renders, roughly 2 minutes of analysis.
- 10,000 URLs across 5 templates: 5 browser renders, roughly 30 seconds.

You analyze one instance of each template and the resulting optimization profile applies to every URL that hashes to it. Each profile carries a TTL (24 hours by default); once it expires the entry is dropped on the next lookup and the template is re-analyzed from the next request that hits it, not per-URL.

This is deliberately conservative. Attribute values are ignored on purpose: a class toggle driven by a CSS variable, an A/B-tested data attribute, or a per-product modifier class would otherwise fragment one logical template into dozens of structural variants and defeat the amortization. The trade-off: if two pages genuinely render different above-fold CSS but share a tag hierarchy, structure-hashing will group them together. The fallback floor is always the heuristic pipeline: when no browser profile is available for a page, the in-process heuristic critical-CSS extractor runs instead, so a page that does not match its template's profile still gets optimized.

## The profile is keyed by template, not URL

When the browser tier finishes analyzing a representative URL, it writes an `OptimizationProfile`. The profile is stamped with the template hash (as a 16-digit hex string), carries the analyzed URL alongside it for provenance, and holds one `ViewportProfile` per breakpoint. `OptimizationProfile::ToJson` serializes it like this:

```json
{
  "version": 1,
  "template_hash": "00000000a1b2c3d4",
  "analyzed_url": "/products/example-widget",
  "mobile": {
    "critical_css": "body{margin:0}header{...}...",
    "lcp_selector": "img.product-hero",
    "lcp_url": "/images/widget-hero.jpg",
    "above_fold_selectors": ["img.product-hero", "img.logo"],
    "below_fold_selectors": ["img.review-thumb", "img.related-1"],
    "image_dimensions": [
      {"selector": "img.product-hero", "rendered_width": 600, "rendered_height": 400, "natural_width": 1200, "natural_height": 800}
    ],
    "css_coverage_ratio": 0.10,
    "total_css_bytes": 42000,
    "unused_css_bytes": 37800
  },
  "tablet": { "...": "..." },
  "desktop": { "...": "..." },
  "preload_hints": [
    {"url": "/images/widget-hero.jpg", "as": "image"}
  ],
  "defer_safe_scripts": ["/js/analytics.js"],
  "created_at": 1718236800,
  "expires_at": 1718323200
}
```

Two details matter for reuse. First, the LCP and above/below-fold data is expressed as CSS selectors, not as a snapshot of one page's bytes — `img.product-hero` and `img.logo` are stable across every product URL, even though the hero image behind that selector changes per product. The profile describes the template's structure, so it applies to siblings without modification. Second, the profile carries all three viewports — mobile (375×667), tablet (768×1024), desktop (1440×900) — because CSS extraction and page analysis run once per viewport per template, and the worker selects the viewport profile that matches the request's capability mask. One template analysis populates all three breakpoints.

The profile lives in the same cache the rest of the pipeline uses, under a synthetic key derived from the template hash. `OptimizationProfile::CacheUrl` formats the URL as `__pagespeed_profile__/{016x}` under the fixed `__internal__` hostname, and it is written as the `kBrowserProfile` sentinel variant (`SentinelId::kBrowserProfile`, value `0x5C`). Keying on the hash, not the URL, is what makes one entry serve every sibling.

When a request for `/products/some-other-widget` comes through and the worker optimizes that HTML, it does not need a fresh render. It hashes the structure with `HashStructure`, calls `LookupProfile` with the resulting hash, and pulls the existing profile. The expensive part — the browser — already ran, for a sibling, possibly minutes ago.

For how the heuristic critical CSS that runs before any of this works, see [/blog/critical-css-heuristics/](/blog/critical-css-heuristics/); for why the browser tier exists at all and how it replaced the old beacon, see [/blog/critical-css-beacon-to-headless-history/](/blog/critical-css-beacon-to-headless-history/).

## Dedup at the queue, not at the render

Grouping URLs into templates only saves work if the queue refuses to render templates it has already seen. That is the job of the analysis queue.

The browser tier sits entirely off the request path. nginx serves from cache on every request; when the worker processes a notification for a page, it queues that page for browser analysis as a background step. Amortization is enforced in two places. First, the worker only enqueues when `LookupProfile` misses — if the template already has a stored profile, it reuses it and does not queue anything. Second, the queue itself deduplicates by template hash: `AnalysisQueue::Enqueue` keeps a set of pending template hashes and returns `false` for any item whose template is already in flight. Between the two, there is nothing to learn by rendering a fourth blue-widget page when the template's profile already exists or is already being computed.

The effect is that the queue does work proportional to the number of distinct templates, not the number of URLs. Feed it 100,000 product URLs and it renders one, recognizes the next 99,999 as members of an already-profiled template, and drains. The single managed Chrome process — recycled after a configurable number of pages (default 100) to contain memory growth — never sees the redundant 99,999. Analysis runs one item at a time off the queue, so the browser's load is bounded by the count of distinct templates, not by traffic.

It also bounds the failure surface. If Chrome crashes or times out on a render, the manager restarts it on a timer and the queue moves on, while pages keep getting the heuristic-extracted critical CSS until a profile exists. The browser tier is strictly additive: its worst case is "serve the fast heuristic version," never "serve nothing" and never "block the request." The dedup logic is what makes that additive layer cheap enough to leave on.

A note on scope: the loop described here is wired into the worker. On each HTML notification the worker computes the template hash, calls `LookupProfile`, applies the profile's per-viewport critical CSS and safe-to-defer scripts when one is found, and otherwise enqueues an analysis — the dedup, the store, and the reuse all run in the shipping worker. The browser tier is gated behind configuration, and the heuristic pipeline is the always-available floor underneath it. What this post does not give you is a benchmark: the throughput numbers above (100,000 URLs to 15 renders, the rough minutes) are the arithmetic the amortization implies, not a measurement on your catalog. The mechanism is real; the figures are illustrative.

## Related

- [/blog/critical-css-heuristics/](/blog/critical-css-heuristics/) — the fast, in-process heuristic extractor that runs first and serves as the fallback floor.
- [/blog/critical-css-beacon-to-headless-history/](/blog/critical-css-beacon-to-headless-history/) — why critical CSS moved from a JS beacon to async headless rendering.
- [/blog/server-side-critical-css-nginx/](/blog/server-side-critical-css-nginx/) — where critical CSS is injected in the nginx + worker split.
- [/blog/css-cache-inlining-for-coverage-api/](/blog/css-cache-inlining-for-coverage-api/) — how cached CSS feeds the Coverage API that the profile is built from.
- [/blog/visual-regression-gating-optimizations/](/blog/visual-regression-gating-optimizations/) — the screenshot gate that vets a browser-validated variant before it replaces the heuristic one.
- [/how-it-works/css-parsing/](/how-it-works/css-parsing/) — how CSS is parsed in the optimization pipeline.
- [/docs/browser-analysis/](/docs/browser-analysis/) — configuring the asynchronous browser-analysis tier.

If you run a large catalog or a CMS where most of the page count comes from a few templates, this is the design that decides whether browser-validated critical CSS is worth the compute. Pull the [mod_pagespeed worker and nginx images](/download/) and read [/docs/browser-analysis/](/docs/browser-analysis/) for the flags that gate the analysis tier: start with the heuristic pipeline running everywhere, then layer the browser tier on for the templates that justify it. It is licensed under Apache-2.0 and free to run, so you can profile your own templates and see the dedup math on your own URLs.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

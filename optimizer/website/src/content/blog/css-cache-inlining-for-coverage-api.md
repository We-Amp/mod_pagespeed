---
title: "Chrome's Coverage API: inlining cached CSS for critical CSS"
description: "How mod_pagespeed feeds the Chrome Coverage API critical CSS: the worker inlines cached stylesheets into a network-blocked sandbox to see real usage."
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ["critical-css", "headless-chrome", "css", "caching", "architecture"]
draft: false
product: '2.0'
---

Run a critical CSS extraction against modpagespeed.com and the Chrome Coverage API hands back a clean, confident number: **0 bytes of critical CSS at 0.0% coverage.** Three viewport extractions, three zeroes. The pipeline ran, Chrome started rule-usage tracking, the Coverage API answered accurately, and the answer was nothing.

The bug is not in the extractor. It is in what Chrome was allowed to see.

## Why the Chrome Coverage API returns 0% for critical CSS

The optimizer worker computes critical CSS by loading a page's HTML into a fully sandboxed Chrome tab and reading the CSS Coverage API (`CSS.startRuleUsageTracking` → `takeCoverageDelta` → `stopRuleUsageTracking`). That tab is locked down hard: network offline, JavaScript disabled, every fetch blocked. The lockdown is deliberate. Browser analysis runs over content the worker pulled from cache, and the URLs inside that content are not trustworthy. An open network stack inside the analysis tab is an SSRF primitive pointed at your internal services. So we close it.

The cost of closing it shows up the moment a page uses `<link rel="stylesheet">` — which is to say, virtually every production site. Chrome cannot fetch the external stylesheet, so the document it analyzes has no CSS attached. The Coverage API reports usage against rules it can see, and it sees none. Zero bytes is the correct answer to the wrong document.

The Astro-generated HTML behind modpagespeed.com is the canonical reproduction: all CSS arrives via external links, so all three viewport extractions return 0.0%. The extractor is doing exactly what it was told. We were just telling it to measure an empty stylesheet.

The tempting fix — relax the sandbox — is the one we rejected. Allowing same-origin network access reopens the SSRF hole the sandbox exists to close, because attacker-influenced HTML can name any internal URL. An allowlist has the same problem: the HTML controls the hrefs. A Service Worker shim needs network to register and JavaScript to run, both disabled. So the sandbox stays exactly as paranoid as it is. We change what reaches it, not what it can reach.

## Inline the CSS we already have

The worker already holds the CSS. nginx proxied those stylesheets, and the [Cyclone cache](/how-it-works/metadata-cache/) holds the version it last saw. So before handing HTML to Chrome, the optimizer worker enriches it: parse the `<link rel="stylesheet">` tags, look each one up in the cache, and inject a `<style>` block carrying the real CSS. Chrome's network stays offline. The bytes arrive through the document instead of through a socket.

The enrichment runs inside `RunAnalysis()`, between the cache read and the call to the extractor. It lives in its own file (`css_cache_inliner.cc`) and takes a lookup callback rather than a cache handle:

```cpp
// css::CssLookupFn, from lib/css/css_import_flattener.h:
//   std::function<std::optional<std::string>(std::string_view url)>

std::string InlineCachedStylesheets(
    std::string_view html,
    std::string_view page_url,
    const css::CssLookupFn& lookup,
    CssInliningStats* stats = nullptr);
```

That signature is not an accident. It reuses the `css::CssLookupFn` type the `css::FlattenImports` flattener already defines, decouples the logic from the cache implementation, and makes unit tests trivial — a lambda returning preset strings, no live cache required. At the call site the lambda wraps `cache_->ReadBestAlternate(...)` and hands back the cached bytes (or `nullopt` when nothing is cached).

The algorithm is deliberately small. For each scanned stylesheet link, up to a cap of 50:

- Skip `data:` and `javascript:` URIs — neither is a cache-backed resource.
- Resolve the `href` against the page URL to an absolute URL, query string included.
- Look it up. If it is not cached, skip it — Chrome would not have fetched it either, so we are no worse off.
- Flatten `@import` chains through `css::FlattenImports()`, which already handles relative-URL resolution, circular-dependency detection, a depth limit, and media-query wrapping. See [the import-flattening edge cases](/blog/flatten-css-imports-edge-cases/) for why that step is harder than it looks.
- If the original `<link>` carried a `media` value other than `all`, and that value passes a character check (no `{ } ; < > \ ' " @`, so a crafted `media` attribute cannot break out of the wrapper), wrap the CSS in `@media <value> { ... }` so conditional rules stay conditional.
- Collect a `<style>` block per stylesheet, in source order, injected before `</head>`.

One decision saved an entire class of bugs: we do **not** remove the original `<link>` tags. With the network blocked they are inert, so they cause no double-loading. Stripping them would have meant regex surgery on live HTML — multiline tags, attribute-order variance, `<link>` inside `<noscript>` — for zero functional gain on HTML that is never user-facing. The injected `<style>` blocks carry the content; the dead links sit there harmlessly.

Per-stylesheet blocks (rather than one concatenated blob) preserve cascade source order more faithfully. The interleaving between newly-inlined external CSS and any pre-existing inline `<style>` can still differ from production, and that is fine for this purpose: the Coverage API reports every matching rule regardless of which one wins the cascade. We need the used-vs-unused split, not the final computed pixel.

## The CSS came from cache, so treat it as hostile

Cached CSS is not trusted input. It can contain whatever the origin served, and the analysis HTML is assembled from cache, so anything we inline into a `<style>` element has to be sanitized first. The optimizer worker reuses the exact pattern already in `html_css_injector.cc`:

1. Strip null bytes, so a `\0` cannot smuggle a `</style>` past the next check.
2. Scan the sanitized CSS for `</style` case-insensitively.
3. If it is present, skip that stylesheet entirely — do not inline it.

A stylesheet that can break out of its `<style>` element is a stylesheet that does not get inlined. Coverage for that one resource stays at zero, which is the correct trade against letting attacker-controlled bytes escape into the document context. Two more bounds keep a hostile or merely enormous origin from exhausting memory: individual stylesheets above 2 MB are skipped, and if the enriched HTML would exceed 10 MB the whole step falls back to the original, un-enriched HTML. Falling back means 0% coverage — the same result as before the fix, never worse.

A note on `url()` references, because it looks like a hole and is not one. Relative `url()` paths for backgrounds and fonts break when CSS moves from an external file into an inline block, because the base URL changes. That does not matter here. The Coverage API tracks selector matching against the DOM, not resource loading. Fonts will not load (network is off), background images will not render (not needed), and `@font-face` rules are still marked used based on `font-family` references in computed style, not on whether the font binary downloaded. We are measuring which rules apply, not painting a frame.

The behavior table is short. All same-origin CSS cached: real coverage. Some cached: partial coverage from what we have. Nothing cached, or cross-origin CDN CSS that never entered our cache: still 0%, unchanged from today. By the time browser analysis runs — it is enqueued after HTML processing, and nginx caches same-origin CSS as the origin sends it — that first case is the common one.

This is a correctness fix, not a feature, which is why it ships without a feature flag. The old path produced no usable data; the new one is purely additive. New stats counters (`css_inlining_stylesheets_found`, `css_inlining_stylesheets_cached`, `css_inlining_bytes_inlined`, and the per-run attempt counter) surface in `BROWSER-STATUS` and the Prometheus `/v1/metrics` endpoint, so you can see how much CSS actually reached Chrome on any given analysis. Browser analysis remains gated behind `--enable-browser-analysis`, off by default, and still requires a Chrome binary.

The longer-term direction for the CSS extractor is to stop manipulating HTML at all and serve cached resources through CDP's `Fetch.fulfillRequest` — fail-the-request becomes serve-from-cache-or-fail — which restores correct `url()` resolution and covers fonts and images through the same path. That is not how `BrowserCssExtractor` works today: it fails every intercepted request and gets its CSS from the inlined `<style>` blocks instead. The sibling analyzers already prove out the alternative — `ScriptCoverageAnalyzer` and the shared page analyzer fulfill intercepted requests from an in-memory cached-resource map rather than inlining — so the move for CSS is to adopt that existing mechanism once the inlining pipeline is validated in production. For now, inlining is the smaller, lower-risk change that gets the Coverage API real CSS to chew on.

## Related

- [Server-side critical CSS delivery in nginx](/blog/server-side-critical-css-nginx/) — how the extracted critical CSS gets in front of users once this pipeline produces it
- [The heuristics behind critical CSS extraction](/blog/critical-css-heuristics/)
- [Template hashing for critical-CSS reuse](/blog/template-hashing-critical-css-reuse/)
- [Flattening CSS @import chains: the edge cases](/blog/flatten-css-imports-edge-cases/)
- [Removing unused JavaScript with the Chrome Coverage API](/blog/remove-unused-javascript-chrome-coverage/) — the JS-side sibling of this pipeline, fulfilling intercepted requests instead of inlining
- [From beacon to headless: the history of critical CSS](/blog/critical-css-beacon-to-headless-history/)
- [How async rewriting works](/how-it-works/async-rewriting/) and [browser analysis](/docs/browser-analysis/)

If you want to see real coverage numbers instead of three zeroes, the browser-analysis pipeline ships in [mod_pagespeed](/download/); turn it on with `--enable-browser-analysis` and watch the `css_inlining_*` counters in `BROWSER-STATUS`. The [browser-analysis docs](/docs/browser-analysis/) cover the gate, the Chrome dependency, and how the extracted critical CSS feeds back into rewriting. It is licensed under Apache-2.0 and free to run, so you can prove the coverage numbers on your own content.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

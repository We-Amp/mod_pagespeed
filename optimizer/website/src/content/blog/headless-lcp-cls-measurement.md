---
title: 'Headless LCP and CLS measurement to drive optimization'
description: "Headless LCP and CLS measurement in mod_pagespeed: the optimizer worker's PerformanceObservers capture the LCP element and shifts to drive preloads."
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'headless-chrome', 'performance', 'deep-dive']
draft: false
product: '2.0'
---

The first `<img>` in the body is not the LCP element. It often is, which is why the heuristic guess works often enough to be dangerous. On a product page the hero is frequently a CSS `background-image`; on a blog post it is the cover photo three sections down; on a hydrated SPA the largest paint lands on an element that did not exist in the source HTML at all. If you preload the wrong resource you have spent a high-priority connection slot on something that was never on the critical path. Headless LCP and CLS measurement exists to replace that guess with what Blink actually painted.

The optimizer worker does this in the same `chrome-headless-shell` render it uses for critical-CSS coverage, off the request path. The mechanism is small and worth reading literally: a script injected before any page script runs sets up two `PerformanceObserver`s, and after the page settles the worker reads back what they recorded. This post walks the injected JavaScript field by field, then shows where the captured LCP selector and URL land in the stored optimization profile and what consumes them. The source is `src/browser/page_analysis.cc` and `src/browser/optimization_profile.{h,cc}`.

## Inject the LCP and CLS observers before the page can touch them

The naive way to measure LCP is `Runtime.evaluate` after load. That loses everything: a `PerformanceObserver` created after paint with `buffered: true` can recover buffered entries, but the document object that holds your observer is destroyed and recreated across navigation, so a freshly-evaluated observer is racing the very lifecycle it is trying to measure. The optimizer worker sidesteps this by registering the observer through CDP's `Page.addScriptToEvaluateOnNewDocument`, which runs the script on every new document before the page's own scripts. The comment in `page_analysis.cc` states the reason directly: this "creates PerformanceObservers for LCP and CLS that survive navigation (unlike Runtime.evaluate which is destroyed)."

That ordering also buys a security property. The injected script captures `JSON.stringify` into `window.__PS_stringify` before any page code can run, so a hostile page that overrides `JSON.stringify` to smuggle data into the result cannot tamper with collection. The collection script later reads `window.__PS_stringify || JSON.stringify` rather than calling the global directly. The render itself is locked down independently: the session sets `Network.emulateNetworkConditions` offline and enables `Fetch.enable` with a catch-all pattern, so the page can only be served bytes the worker already holds in cache (how that legacy path blocks everything else is covered in [air-gapped headless fetch with SSRF pinning](/blog/air-gapped-headless-fetch-ssrf-pinning/)).

The injected script seeds two globals with fully-typed defaults so the reader never has to deal with `undefined`:

```js
window.__PS_LCP = {selector: '', url: '', tag: '', size: 0, time: 0};
window.__PS_CLS = {total: 0, sources: []};
```

The LCP observer subscribes with `{type: 'largest-contentful-paint', buffered: true}` and, for every entry, overwrites `window.__PS_LCP` wholesale. LCP entries arrive monotonically (each candidate is at least as large as the last), so last-write-wins is exactly what you want; the final value is the largest contentful paint. For each entry it records `url` (populated by Blink for image LCP, empty for a text node), `size`, and `time` from `e.startTime`. When `e.element` is present it reads `tag` from `tagName` and builds a `selector` from the tag plus `#id` if there is an id plus `.class.class` from the trimmed, whitespace-split class list. That selector-building is plain string concatenation on the rendered element, not a CSS-engine query, which is why it stays cheap.

## What the CLS observer records, and what it deliberately does not

The CLS observer subscribes with `{type: 'layout-shift', buffered: true}` and is stricter about what counts. It skips any entry where `e.hadRecentInput` is true, so a shift the user caused by tapping a button is not blamed on the page. For every counted entry it adds `e.value` to `window.__PS_CLS.total` and, for each `source` in the entry, pushes `{selector, value}` onto `window.__PS_CLS.sources`. The source selector is built more conservatively than the LCP one: tag name plus `#id` only, no class list. So `__PS_CLS.total` is the cumulative shift score and `sources` is the list of nodes that moved, each tagged with the shift value it contributed.

What the shipped collector does not capture is worth naming. An earlier design recorded `previousRect` and `currentRect` per source for before/after geometry; the shipped `kObserverScript` records only `selector` and `value`. The code is narrower than that, and the code is the truth. There are no shift-magnitude thresholds, no error budgets, no rollback rules in this file. Those belong to the visual-regression gate and the consumer loop, not to the measurement step. Measurement here does one thing: record what Blink reported.

After CDP signals `Page.lifecycleEvent("networkIdle")`, the legacy perf path runs `CollectResults`, which evaluates a second injected script. That script returns `lcp`, `cls`, `images`, and an `fcp` value pulled from `performance.getEntriesByType('paint')` for `first-contentful-paint`. For each `<img>` it captures the rendered rect via `getBoundingClientRect()`: `above_fold` is `rect.top < window.innerHeight`, plus rounded `rendered_width`/`rendered_height` and the intrinsic `natural_width`/`natural_height`. The C++ side parses that JSON into `PageAnalysisResult`: `analysis.lcp.selector`, `lcp.url`, `lcp.element_tag`, `lcp.size`, `lcp_ms` from `time`; `cls_total` and a vector of `ClsSource{selector, shift_value}`; `fcp_ms`; and the per-image `ImageInfo`. That struct is the raw measurement, one render at one viewport.

## Where the measurement lands and what reads it

A `PageAnalysisResult` is per-render. The durable artifact is the `OptimizationProfile`, defined in `optimization_profile.h`. It holds three `ViewportProfile`s, one each for `mobile` (375x667), `tablet` (768x1024), and `desktop` (1440x900), matching the viewports the analyzer renders. Inside each `ViewportProfile` the measured LCP lands in two fields: `lcp_selector` and `lcp_url`. The CLS data is not stored per-field in the profile; what survives into the profile is the LCP identity and the geometry, the inputs the worker needs to make a layout decision, plus `critical_css`, `above_fold_selectors`/`below_fold_selectors`, `image_dimensions`, and the CSS coverage counters.

The profile is serialized by `ToJson`, which stamps `version: 1`, the `template_hash`, the `analyzed_url`, the three viewport blocks, a cross-viewport `preload_hints` array, `defer_safe_scripts`, and `created_at`/`expires_at`. It is stored in cache under a synthetic key the header spells out: URL `__pagespeed_profile__/{hex(template_hash)}` built by `OptimizationProfile::CacheUrl`, hostname `__internal__`, sentinel `kBrowserProfile (0x5C)`. Keying on the template hash rather than the URL is what makes the render reusable: one render answers for every URL that shares the template. That hashing scheme has its own write-up in [template hashing for critical-CSS reuse](/blog/template-hashing-critical-css-reuse/).

Two consumers read these fields. `lcp_url` is the resource you want the browser fetching first, so it feeds the preload path: the profile's `preload_hints` (each a `{url, as, type}` `PreloadHint`) are emitted as `<link rel="preload">` and over the 103 Early Hints response, the mechanism described in [sentinel cache keys and 103 Early Hints](/blog/sentinel-cache-keys-and-103-early-hints/). Knowing the real LCP element also means the worker keeps it off the lazy-load list and gives it `fetchpriority="high"` instead of demoting it. `lcp_selector` and the stored `image_dimensions` drive the layout decisions: a known LCP selector tells the transform which element must not shift, and the rendered dimensions are what let the worker stamp width/height to prevent the layout shift the CLS observer would otherwise record on the next render.

This is the optimizer measuring its own targets, which is a different job from telling a developer how to fix theirs. If you want the remediation guides, the WordPress LCP walkthrough is at [fix LCP on WordPress](/blog/fix-lcp-wordpress-2026/) and the conceptual metric pages live under [Core Web Vitals](/core-web-vitals/). What runs here, on every analyzed template, is the measurement that those fixes are validated against. The closed loop carries its own history: the original mod_pagespeed measured LCP by shipping a beacon to the real browser and waiting for it to phone home, a path covered in [from critical-CSS beacon to headless history](/blog/critical-css-beacon-to-headless-history/). The 2.0 rebuild moves that measurement into a controlled render so the answer is ready before the first user arrives, and gates the resulting variant through [visual-regression gating](/blog/visual-regression-gating-optimizations/) before it ships.

## Related

- [Template hashing for critical-CSS reuse](/blog/template-hashing-critical-css-reuse/)
- [Sentinel cache keys and 103 Early Hints](/blog/sentinel-cache-keys-and-103-early-hints/)
- [Visual-regression gating of optimizations](/blog/visual-regression-gating-optimizations/)
- [Fix LCP on WordPress](/blog/fix-lcp-wordpress-2026/)
- [From critical-CSS beacon to headless history](/blog/critical-css-beacon-to-headless-history/)
- [Browser analysis](/docs/browser-analysis/)
- [Core Web Vitals: LCP](/core-web-vitals/lcp/)
- [Core Web Vitals: CLS](/core-web-vitals/cls/)

If you run a reverse proxy and want to see what your own templates actually paint, the worker that does this measurement ships in the 2.0 [download](/download/); the [browser analysis docs](/docs/browser-analysis/) cover enabling the headless tier. It is licensed under Apache-2.0 and free to run, so you can read the measured LCP and CLS for your own templates.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

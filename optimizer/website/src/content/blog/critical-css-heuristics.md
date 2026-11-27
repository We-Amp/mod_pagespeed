---
title: 'Critical CSS without a browser, or with headless Chrome'
description: 'How the mod_pagespeed optimizer worker inlines critical CSS two ways: static heuristics in under 5ms per page, or an optional headless-Chrome path.'
date: 2026-02-08
author: 'Otto van der Schaaf'
tags: ['critical-css', 'css', 'performance', 'headless-chrome', 'core-web-vitals', 'deep-dive']
draft: false
product: '2.0'
lastUpdated: 2026-10-22
---

## Two ways to find the critical CSS

Critical CSS -- the minimal set of styles needed to render above-the-fold content -- removes render-blocking CSS from the critical path, one of the largest single wins for First Contentful Paint. Inline it into the HTML `<head>` and you eliminate render-blocking stylesheet requests, letting the browser paint meaningful content sooner. The hard part is knowing which rules are actually critical.

There are two ways to answer that. You can render the page in a real browser and watch which rules apply to visible elements. Or you can read the HTML and CSS statically and reason about structure. The first is more precise. The second is faster by three orders of magnitude and needs no external process.

The optimizer worker ships both. Heuristics are the always-on default: every page gets critical CSS extracted in single-digit milliseconds with no dependencies. Headless Chrome is an opt-in accuracy upgrade that runs off the request path and caches its results per template. This post covers how each path works, and how they fit together.

## Why headless Chrome can't sit on the request path

The standard critical-CSS tools today -- Critical, Penthouse, CriticalCSS -- all spin up headless Chromium, navigate to the page at several viewport sizes, and record which rules applied to visible elements. It works, and it's precise. On the request path, the cost is prohibitive.

A headless Chromium process consumes 200-400 MB of memory and takes 2-5 seconds per page to render and evaluate. You cannot do that synchronously while a visitor waits for HTML. You also cannot do it once and forget it: Chromium updates shift element geometry and break automation APIs, so the extraction is a moving target that needs babysitting.

For a system like mod_pagespeed that [extracts critical CSS server-side at the nginx layer](/blog/server-side-critical-css-nginx/) with no application changes, requiring a headless browser *for every request* would be a non-starter. So the default path uses no browser at all. The browser, when you want it, runs somewhere else entirely.

## The heuristic path: critical CSS without a browser

Instead of rendering the page, the worker scans the HTML statically and matches CSS selectors against the DOM structure using tuned heuristics. The pipeline runs in four steps, none of which touch an external process.

First, when the nginx interceptor records a cache miss for an HTML page, it stores the original response at the default capability mask and sends a fire-and-forget notification to the worker over a Unix socket. The worker reads the HTML back from the shared Cyclone cache and feeds it to the `HtmlScanner`, which parses the document and collects elements with their tag names, IDs, classes, and DOM depth -- without modifying the HTML.

Second, the scanner gathers all CSS available for the page: inline styles from `<style>` tags, plus external stylesheets referenced by `<link rel="stylesheet">`. External CSS is looked up from the Cyclone cache, where a previous request may already have stored it. The worker combines everything into a single input for the extractor.

Third, the `CriticalCssExtractor` evaluates every CSS rule against the collected elements and the configured heuristics. It produces a filtered subset of rules -- the critical CSS -- along with statistics on how many rules were total versus retained.

Finally, `InjectCriticalCss` inserts the extracted CSS into the HTML as a `<style data-pagespeed-critical>` block. The injection follows a fallback chain: before `</head>`, then before `</body>`, then after `<head>`, and as a last resort at the document start. The modified HTML is written back to the cache as a variant at the requesting client's capability mask.

The whole pipeline completes in single-digit milliseconds, with no browser binaries and no network calls beyond cache reads.

## Which selectors are above the fold

The heuristics in `CriticalCssExtractor` use a layered set of rules, each keyed to a common structural pattern.

Universal selectors (`*`), `html`, `body`, and `:root` are included unconditionally. These set base typography, box-sizing resets, CSS custom properties, and background colors that affect every page. Excluding them would cause a visible flash of unstyled content on almost any site.

The first 300 elements inside `<body>` (configurable via `CriticalCssConfig::max_elements`) are treated as above-the-fold, together with the root `<html>` element. Any CSS rule whose selector matches one of these elements is included. The budget is counted from `<body>` because nothing in `<head>` paints, and a modern page spends dozens of elements there; it is sized for utility-class markup, where a header, its hidden menus and a hero run to a few hundred elements. Elements that a `position: fixed` rule selects are above the fold wherever they sit in the document, so they and their contents are included too — the chat launcher or cookie banner at the end of `<body>` is on screen from the first paint.

Elements whose tag names, class names, or IDs contain `header`, `nav`, `hero`, `banner`, `masthead`, or `above-fold` are always treated as critical, regardless of position in the DOM. These conventions are near-universal across CSS frameworks and custom sites, so matching on them catches above-the-fold content that sits deeper in the tree behind wrapper elements.

On the exclusion side, elements deeper than 10 levels in the DOM tree are assumed to be nested inside below-the-fold content -- deeply nested cards, accordion items, comment threads. Rules matching only these deep elements are excluded. The depth limit of 10 is generous; most header and navigation structures sit within 4-6 levels.

Elements matching `footer`, `lazy`, `defer`, `below-fold`, or `lazyload` in their classes or IDs are excluded too. These are overwhelmingly associated with content not visible on initial render. Entire `@media print` blocks are excluded as well, since they never affect screen rendering.

## The limit of static analysis

In testing across a corpus of real-world sites -- e-commerce product pages, WordPress blogs, news portals, single-page-app shells -- the heuristic extractor typically retains 15-35% of total CSS rules as critical. The extraction step itself (excluding HTML parsing and cache I/O) completes in under 5 milliseconds for stylesheets up to the 2 MB limit enforced by the worker's `max_css_size`.

The impact on Core Web Vitals is meaningful. Inlining critical CSS removes render-blocking requests, which directly improves First Contentful Paint. On pages with large external stylesheets (common with Bootstrap, Tailwind, or custom frameworks), FCP improvements of 200-500 ms are typical on 3G connections. The worker also [stores preload hints at a reserved cache key](/blog/sentinel-cache-keys-and-103-early-hints/) (mask `0xFFFFFFFF`), letting the nginx interceptor send `103 Early Hints` responses with `Link: <url>; rel=preload; as=style` headers on subsequent misses, stacking the improvement further.

The trade-off is precision. A real browser knows exactly which pixels are visible at paint; heuristics infer it from structure. The extractor will sometimes include rules that are not strictly above-the-fold -- a `.hero-secondary` rule that only appears after scrolling, say. This over-inclusion is deliberate. A few extra bytes (under 2 KB) is cheap; missing a critical rule causes a flash of unstyled content. The heuristics err toward including too much.

There is also dynamically-inserted content. If JavaScript injects a `<div class="popup-overlay">` after load, the static extractor has no knowledge of it -- but such content is by definition not above-the-fold on initial render, so excluding its CSS is correct.

Most of the time that's the right trade: fast, dependency-free, accurate enough. When you want the browser's precision, you can have it.

## The headless-Chrome path (opt-in, higher accuracy)

Turn on `--enable-browser-analysis` and the worker gains a second extractor, `BrowserCssExtractor`, that reads the *true* critical CSS straight from Chrome instead of inferring it. It measures the same signal [the original mod_pagespeed collected with a client-side beacon](/blog/critical-css-beacon-to-headless-history/) — now taken from a headless render off the request path instead of from your visitors' browsers.

The worker loads the page, lets it paint, and asks Chrome which rules were live at that moment. Concretely, it drives headless Chrome (or `chrome-headless-shell`) over the Chrome DevTools Protocol, calls `CSS.startRuleUsageTracking`, and then captures `CSS.takeCoverageDelta` at the **First Contentful Paint** lifecycle event. The rules active at FCP *are* the critical CSS -- not a heuristic approximation of it. Before the page loads, the worker resolves `<link rel="stylesheet">` tags against the Cyclone cache and inlines them as `<style>` blocks, so [Chrome's Coverage API reports real percentages](/blog/css-cache-inlining-for-coverage-api/) instead of 0% for external sheets.

It runs across three viewports -- **Mobile (375x667)**, **Tablet (768x1024)**, and **Desktop (1440x900)** -- producing per-viewport critical CSS. Mobile and desktop layouts often differ on what's above the fold, so each gets its own extraction.

The transport stays lightweight: CDP runs over a `--remote-debugging-pipe` on file descriptors 3 and 4, not a WebSocket. No port to manage, no localhost server to secure. Messages are null-delimited JSON-RPC.

### You pay for it once per template, not per request

Browser analysis stays practical because the cost is [amortized across every page that shares a template](/blog/template-hashing-critical-css-reuse/).

When a page comes in, the worker runs `HtmlScanner::Scan()` and then `TemplateDetector::HashStructure()` to compute an FNV-1a hash of the DOM structure. That hash identifies the page's template. The worker looks up an `OptimizationProfile` keyed by that hash:

- **Profile found:** the browser-extracted critical CSS is used directly. No Chrome involved on this request.
- **No profile:** the page is enqueued for analysis on the main event loop, optimized *now* with the heuristic path, and the browser result is cached as the template's profile for subsequent requests.

So your 50,000 product pages don't trigger 50,000 Chrome runs. They share a handful of templates, each analyzed once, asynchronously, off the request path. The resulting profile is cached with a default TTL of 24 hours (`--browser-profile-ttl 86400`). The analysis queue is bounded (`--browser-queue-size 1000`) so a traffic spike can't pile up unbounded work.

In `worker.cc` the decision is simple: if a browser profile exists and browser critical CSS is enabled, use the browser-extracted CSS; otherwise fall back to the heuristic `CriticalCssExtractor`. Both feed the *same* `InjectCriticalCss` path and emit the same `<style>` block in `<head>`. The injection site doesn't know or care which extractor produced the rules.

### Strictly additive: the heuristic path is the safety net

Browser analysis never makes a page *worse*, because every failure falls back to heuristics:

| Failure                     | Behavior                                       |
| --------------------------- | ---------------------------------------------- |
| Chrome binary not found     | Heuristic only, no retry                       |
| Chrome fails to start       | Retry after 2 seconds                          |
| Chrome crashes mid-analysis | Cancel item, restart Chrome after 2s           |
| Analysis timeout            | Skip item, process next in queue               |
| Queue full                  | Head-drop the oldest queued item               |

In every one of these cases the page still gets optimized -- just with the faster, less-precise heuristic pipeline. You don't lose critical CSS when Chrome has a bad day. You lose the *extra* precision until the next analysis succeeds.

## What it takes to run Chrome (and keep it caged)

Running Chrome raises two questions: footprint and safety.

**Footprint.** The optimizer worker does not bundle Chrome. The Docker release images (`ghcr.io/we-amp/pagespeed-worker`) ship with Chromium pre-installed, so there's nothing to do. Outside Docker, install Chrome or `chrome-headless-shell` and point `--chrome-binary` at it (default `/usr/bin/chrome-headless-shell`). The worker manages Chrome's lifecycle so it can't drift or leak: it recycles the process every `--chrome-recycle-interval` pages (default 100), reads `/proc/pid/status` VmRSS every five seconds and restarts above `--chrome-max-memory` (default 512 MB), and caps queued work at `--browser-queue-size`. A long-running analysis backlog can't accumulate a leaking browser.

**SSRF defense.** Browser analysis works exclusively on *cached* content: the worker inlines the HTML and CSS it already holds and hands that to Chrome. Chrome never makes a live network request, and four independent layers enforce it:

1. `Network.emulateNetworkConditions({offline: true})` -- the browser believes it's offline
2. `Fetch.enable` + `Fetch.requestPaused` -- every outbound request is intercepted and failed
3. `Emulation.setScriptExecutionDisabled({value: true})` -- no JavaScript runs during CSS extraction
4. `--host-resolver-rules="MAP * ~NOTFOUND"` -- DNS itself returns nothing

A page can't smuggle Chrome into fetching an internal admin endpoint or an external tracker, because there is no path to the network for it to take. This matters precisely because the browser is reading content you don't fully control.

## Turning it on

The flags are small, and all of them hot-reload via `PATCH /v1/config` from the web console -- no restart to flip browser analysis on, watch the `browser.profiles_generated` counter climb, and flip it off again.

| Flag                        | Default                          | What it does                                       |
| --------------------------- | -------------------------------- | -------------------------------------------------- |
| `--enable-browser-analysis` | off                              | Enables the browser-analysis pipeline              |
| `--chrome-binary`           | `/usr/bin/chrome-headless-shell` | Path to the Chrome binary                          |
| `--no-browser-critical-css` | (enabled)                        | Disables browser critical CSS (heuristics resume)  |
| `--chrome-recycle-interval` | 100                              | Pages per Chrome instance before restart           |
| `--chrome-max-memory`       | 512                              | Max Chrome RSS in MB before a forced restart       |
| `--browser-profile-ttl`     | 86400                            | Profile cache lifetime in seconds (24h)            |

Browser critical CSS is *on* once analysis is enabled; `--no-browser-critical-css` is the escape hatch if you want browser analysis for other purposes but prefer heuristic critical CSS specifically.

## What's still on the list

Browser analysis closed the heuristic path's biggest gap: real rendering. A few items remain.

The element-count and depth thresholds are still global constants. A future version will let operators tune `max_elements` and `max_depth` per site through the worker configuration, for layouts where 300 elements or 10 levels is the wrong cut.

CSS Container Queries (`@container`) are seeing real adoption for component-level responsive design. The heuristic selector matching will be extended to handle container-query blocks, treating them much like `@media` rules with format-specific inclusion. (The browser path already sees their real effect at paint, since Chrome resolves them natively.)

---

Default to heuristics -- under 5 ms, no dependencies, every page. Turn on headless Chrome when you want true FCP coverage; it's cached per template and falls back to heuristics on any failure, so it can only help.

The selector matching here builds on the same parser internals covered in [how CSS parsing works](/how-it-works/css-parsing/).

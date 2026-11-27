---
title: "How critical CSS extraction works: beacons to headless"
description: "How critical CSS extraction works without a build step: the original beacon model, why CDNs broke it, and how 2.0 moved to heuristics + headless Chrome."
date: 2026-06-13
lastUpdated: 2026-10-22
tags: ["critical-css", "css", "performance", "history", "headless-chrome", "architecture"]
product: "2.0"
draft: false
---
Every critical-CSS tool answers one question: which CSS rules does the browser actually need to paint the top of the page? Get that set right and you can inline a few kilobytes into the `<head>`, defer the rest, and stop a large stylesheet from blocking the first paint. Get it wrong and you ship a flash of unstyled content.

Today most tools answer that question with a headless browser at build time. mod_pagespeed answers it at the server, with no build step. But the way it answers has changed three times, and the middle chapter is the interesting one: before headless Chrome was practical to run server-side, the original Apache PageSpeed project found critical CSS by instrumenting real visitors. That design worked, taught us a lot, and broke in a specific way once a CDN sat in front of it.

This post walks the three eras: the beacon, the failure mode that retired it, and where 2.0 landed.

## The constraint: no browser on the request path

The reason critical CSS is hard server-side comes down to one rule. Only the browser, after layout, knows which rules matched visible elements. A server parsing HTML and CSS sees selectors and a DOM tree, not pixels.

You could render the page in a headless browser to find out. Around 2013 that meant PhantomJS, and the original project considered it. The trouble was cost and fit: a browser process is heavy, slow to render, and a moving target as the engine updates. You cannot put one synchronously on the path of a visitor waiting for HTML, and a reverse-proxy module had no clean way to run one out of band either.

So the project tried something else. If the server can't run a browser, borrow the browsers it already has: the visitors.

## Era one: the beacon

The beacon design, written up by Jan-Willem Maessen for the Apache PageSpeed project in 2013, split page serving into two modes that share most of their machinery.

**Instrumentation mode.** Some fraction of visitors get a page with a small JavaScript snippet injected. After the page paints, that script walks the rendered DOM, works out which CSS rules applied to elements above the fold, and POSTs the result back to a beacon endpoint on the server. (The 2013 design floated GET, POST, and hybrid variants; the shipped filter settled on POST.) The server stores the reported selectors in its property cache, keyed separately for mobile and desktop user agents so the two layouts never mix.

**Rewriting mode.** Once the server has enough beacon data for a page, it stops instrumenting and starts acting on what it learned. It inlines the critical rules into the `<head>` and defers the full stylesheets. No JavaScript measurement runs; the page just gets the optimized treatment from cache.

The server picks the mode per visitor. With no data yet, it instruments. As data accumulates, the probability of instrumenting drops, so a heavily-visited page mostly serves the fast rewritten variant and only occasionally re-measures. It is a feedback loop: the crowd measures the page, the server learns, the crowd gets the faster version.

In mod_pagespeed this filter is `prioritize_critical_css`, and it still works exactly this way in [the module](/docs/css-filters/#prioritize_critical_css). It is not a core filter and it is marked test-first, because it changes rendered HTML and depends on the beacon endpoint being reachable.

### Why the beacon forced same-origin inlining

There is a sharp edge in the beacon model that shaped the whole design. Browser security only lets JavaScript read the rules inside a stylesheet if that stylesheet came from the same origin as the page. A stylesheet served from a separate cookieless asset domain is opaque to script. The beacon walker simply can't see its rules.

That collides with a common PageSpeed setup, where rewritten assets are deliberately moved to a different domain. So in instrumentation mode the filter has to pull any cross-origin stylesheet back inline, as a `<style>` block on the page, purely so the visitor's browser can measure it. Same-origin assets it could leave as links and annotate to skip rewriting during measurement. The instrumented page is therefore not the page you'd ship; it is a measurement rig, and the rig has to bend the asset layout to satisfy the browser's read rules.

### The security shape of accepting data from visitors

Taking optimization input from arbitrary browsers invites abuse, and the design treated it that way. A signed nonce on each instrumented page lets the server reject beacons it didn't solicit. To stop an attacker from injecting rules that aren't on the page, the rewriter only inlines selectors that genuinely appear in the page's own CSS; a beacon can at most point at real rules, not invent them. POST size was capped so a giant payload couldn't be used to hammer the endpoint. None of this is free, but it is the price of crowd-sourced measurement.

## Era two: the failure mode that retired the beacon

The beacon's weakness was not security. It was caching, and it showed up the moment a CDN or other downstream cache sat in front of the server. Anupama Dutta wrote up the problem and the mitigations for the project in 2013.

Walk the loop with a cache in the middle. The server serves one instrumented page. The CDN caches it. Now thousands of visitors get the same cached HTML, which carries the same single nonce. Their browsers all run the beacon and POST back. The server accepts the one beacon whose nonce it recognizes and rejects the rest as replays. Thousands of measurements collapse into one usable data point.

Worse, while that instrumented page sits in the CDN, the server isn't serving the rewritten variant to anyone, and it isn't collecting fresh measurements either. The feedback loop that the whole design depends on starves. The internal traffic numbers in the design doc make it concrete: medium-traffic pages could end up with a handful of usable beacons across an entire day, far too thin to trust.

The mitigations were clever and they were a lot of moving parts. The downstream cache had to be configured with a shared key and taught to occasionally bypass the cache, with separate sampling rates for cache hits and cache misses, so that some requests reached the origin with a header asking for fresh instrumentation. Instrumented responses had to be marked uncacheable so the CDN wouldn't pin them. An incoming instrumentation header from a client had to be stripped so an attacker couldn't force re-measurement. It worked. It also meant the feature only behaved well when the operator coordinated the optimizer and the CDN in lockstep, and got the sampling percentages right.

That is the assessment of the beacon era. It was the right design when running a browser server-side wasn't an option, and it shipped real wins. It also carried a coordination cost that grew with exactly the caching layers high-traffic sites depend on.

## Era three: the 2.0 re-architecture drops the beacon {#era-three-modpagespeed-20-drops-the-beacon}

The 2.0 re-architecture was a ground-up rebuild, and the optimizer worker does not use the beacon at all. No injected measurement script, no property-cache round trip, no CDN coordination, no nonce bookkeeping. It answers the critical-CSS question two other ways, and both run on the server.

The default is **static heuristics**. The worker parses the HTML and matches CSS selectors against the DOM structure: a small budget of the earliest elements is treated as above the fold, structural conventions like `header`, `nav`, and `hero` are always kept, deeply nested and `footer`/`lazyload` content is dropped, and `@media print` is excluded. It runs in single-digit milliseconds with no browser and no network call beyond a cache read, and it biases toward keeping a little too much rather than too little. The full rule set, with the element and depth thresholds, is in [how the critical-CSS heuristics work](/blog/critical-css-heuristics/).

The optional upgrade is **headless Chrome, off the request path**. Turn on browser analysis and the worker drives a real Chrome over the DevTools Protocol, lets the page paint, and reads back the rules that were live at First Contentful Paint across mobile, tablet, and desktop viewports. That is the actual critical CSS, not an inference. It runs against content the worker already holds in cache, with the browser fully cut off from the network, and the result is [cached per page template](/blog/template-hashing-critical-css-reuse/) so a hundred thousand product pages that share a layout cost a handful of Chrome runs, not a hundred thousand. If Chrome is missing or fails, the page falls back to the heuristic path, so the browser can only add precision, never remove a result.

The contrast with the beacon is the whole point. The beacon measured the real browser of a real visitor and paid for it with a cache-sensitive feedback loop. 2.0 keeps the precise-measurement option but moves the browser onto the server, off the hot path, and caches by template. The measurement is no longer coupled to your traffic distribution or your CDN configuration.

### What this means in practice

If you run **the module**, `prioritize_critical_css` is the beacon model, and it still earns its place: it measures real browsers, and on a site without a misbehaving cache in front of it, it works. Mind the beacon endpoint and the downstream-caching interaction described above.

If you run **the mod_pagespeed optimizer worker**, critical CSS comes from heuristics by default and from cached headless-Chrome measurement when you opt in. Nothing is injected into the page to measure it, and there is nothing to coordinate with your CDN. For the server-layer mechanics on nginx, see [server-side critical CSS for nginx](/blog/server-side-critical-css-nginx/).

One boundary applies to all three eras. Inlining critical CSS removes a render-blocking request, which helps First Contentful Paint and often [Largest Contentful Paint](/core-web-vitals/lcp/). It does not fix layout shift or input delay, and no critical-CSS tool guarantees a Core Web Vitals score. It removes one specific bottleneck. For the rest of the request budget, see [reducing TTFB at the server layer](/blog/reduce-ttfb-server-layer-2026/).

## Design background

The beacon and downstream-caching designs summarized here come from the Apache PageSpeed project, authored by Jan-Willem Maessen (critical CSS beaconing) and Anupama Dutta (beaconing with downstream caching) in 2013. We-Amp was an initial committer on Apache PageSpeed alongside the Google engineers who started the project, as it moved toward Apache incubation. The 2.0 re-architecture was an independent rebuild; it inherits the lineage and the lessons, including this one, and made different choices where a decade of hindsight pointed elsewhere.

mod_pagespeed optimizes out of the box, and it is licensed under Apache-2.0 and free to run. To see it run, browse the [feature list](/features/) or install it and watch a page get rewritten.

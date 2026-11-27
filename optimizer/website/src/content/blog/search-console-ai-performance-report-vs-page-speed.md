---
title: "Search Console's AI performance report is not about speed"
description: 'The Search Console AI performance report measures your visibility in AI Overviews and AI Mode, not how fast your pages load. The difference, explained.'
date: 2026-06-09
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['seo', 'search-console', 'core-web-vitals', 'ai-overviews', 'ai-search']
product: '2.0'
---

## Two reports, one word

Google's new Search Console report measures your search visibility inside its generative AI features. It counts how often your pages show up there. It does not measure load speed.

That trips people up because the report is named for "performance," and "performance" is also the word PageSpeed Insights, Lighthouse, Core Web Vitals, and mod_pagespeed use for how fast your pages load and how stable they are while loading. Same word, two unrelated measurements.

I'm an Apache PageSpeed committer: I helped build ngx_pagespeed, maintained mod_pagespeed and drove its Apache incubation, and now build its maintained continuation, mod_pagespeed 2, at We-Amp B.V., so the speed half is the half I work on. The visibility half is Google's new report, worth understanding on its own terms.

## What Google actually announced

On June 3, 2026, Google's Search Central blog [introduced AI performance reports in Search Console](https://developers.google.com/search/blog/2026/06/gen-ai-performance-reports). The report shows how your site appears inside Google's generative AI features: AI Overviews, AI Mode, and generative AI in Discover.

The data it surfaces:

- **Impressions** — how often your pages appeared in those AI features
- **Pages, countries, devices, and dates** — the usual Search Console breakdowns
- **No click data yet** — clicks are not included in this version of the report

It is rolling out to UK site owners first.

Until now there was no first-party way to see whether Google's AI surfaces were drawing on your pages. This fills that gap. The announcement does not mention PageSpeed, Lighthouse, Core Web Vitals, or PageSpeed Insights, and it does not link to any of them, because it is not about page speed.

_We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google. Google, PageSpeed, and Search Console are trademarks of Google LLC._

## How to read it without over-reading it

A few things to keep in mind when the report shows up in your account:

- **Impressions are not clicks.** This version has no click data, so do not read impressions as traffic. They tell you your pages were shown in an AI feature, not that anyone clicked through.
- **An empty report outside the UK is a rollout stage, not a penalty.** The data is appearing for UK site owners first. If you see nothing yet, that is where the rollout is, not a signal about your site.
- **It reports visibility, not speed.** The report tells you whether AI surfaces are showing your pages. It says nothing about how fast those pages load.

## The other "performance": how fast your pages load

This is the performance PageSpeed Insights, Lighthouse, and Core Web Vitals measure: load speed and visual stability. It is what mod_pagespeed works on. If that is what you came for, start here.

Run a PageSpeed Insights audit and [map each PageSpeed Insights finding to a mod_pagespeed fix](/analyze/). That tool reads a real audit and tells you which findings the module can address and which it cannot.

For the underlying metrics, the [Core Web Vitals](/core-web-vitals/) hub explains each one, with deeper pages on [Largest Contentful Paint](/core-web-vitals/lcp/) and [Cumulative Layout Shift](/core-web-vitals/cls/). The page on [Interaction to Next Paint](/core-web-vitals/inp/) explains what INP is and why we do not claim to fix it — more on that below.

## What mod_pagespeed actually optimizes (and what it doesn't)

mod_pagespeed rewrites the resources a server sends so pages load faster, with no application code changes. The scope:

**It auto-remediates:**

- **Render-blocking resources** — inlines the critical CSS and loads the rest asynchronously
- **Images** — transcodes to WebP and AVIF, recompresses, and resizes to the rendered size
- **Cache lifetimes** — rewrites static asset URLs with a content hash and serves them with a long, immutable cache lifetime
- **Byte weight** — minifies CSS and JavaScript
- **The LCP-resource subset** — preloads the detected LCP image and defers non-critical scripts

**It does not fix INP.** Interaction to Next Paint is driven by your event-handler JavaScript and framework re-renders. mod_pagespeed cannot rewrite that without risking your application's behavior, so its INP coverage is none.

**It does not fix general CLS.** It handles one case: unsized images. It measures each `<img>` and inserts width and height so the browser reserves space. Layout shift from injected ads, late-loading web fonts, or JavaScript adding DOM after first paint is application-level, and the module leaves it alone.

The point that matters here: mod_pagespeed does not change what Google's AI performance report shows. It does not influence your visibility or ranking in AI Overviews, AI Mode, or Discover. It optimizes how your pages load. That is a different problem from whether AI surfaces cite you.

Here is the comparison in one view:

| | Search Console AI performance report | PageSpeed Insights / Lighthouse | mod_pagespeed |
| --- | --- | --- | --- |
| **What it does** | Reports your visibility in Google's AI features | Audits a page and scores it | Rewrites resources so pages load faster |
| **What it measures or changes** | Impressions in AI Overviews, AI Mode, Discover | Load speed and stability (Core Web Vitals) | Load speed (images, CSS/JS, caching) |
| **Signal** | Search visibility | Load speed | Load speed |
| **Who it is for** | Anyone tracking AI-surface reach | Anyone diagnosing a slow page | Server operators who want speed fixed automatically |

## Where the two meanings touch (and where they don't)

There is one connection between speed and visibility, and it is narrow. Faster, lighter pages that render their content server-side are read more reliably by the crawl, render, and index path that feeds every Google surface, AI features included. If an AI crawler does not run JavaScript, a page that only assembles its content client-side reads as nearly empty.

To be clear about what that is not: it is not a claim that page speed improves your AI Overviews visibility. Google's report does not score speed, and there is no published data tying load speed to AI-surface ranking. The connection is about whether your content is reachable at all, not about where it ranks.

That reachability gap is a separate thing we did measure. [40% of the top 1,000 sites serve a near-empty page to AI crawlers](/blog/can-ai-read-your-website/) that do not execute JavaScript — the content is there for a browser but absent for the crawler. You can check your own pages with [RenderPeek](/ai-readability/). Closing that gap is a server-side problem: [handling AI crawlers at the origin](/blog/agentic-web-at-the-origin/) — verifying which bots are real, serving them a clean rendered copy of the page — is a different axis again from how fast the page loads.

It shows that speed and AI-readability are different axes. A fast site with good Core Web Vitals can still be invisible to a non-JS AI crawler if its content is rendered in the browser. A slower site that renders fully server-side can be perfectly readable. One is about how quickly bytes arrive; the other is about whether the content is in the HTML at all.

Core Web Vitals, meanwhile, remain a ranking signal in classic search. That is separate again from this new AI-visibility report, which does not score speed.

## Common questions

**Does Google's new AI performance report measure page speed?**
No. It measures search visibility — impressions inside Google's generative AI features (AI Overviews, AI Mode, and generative AI in Discover). It does not measure how fast your pages load.

**Is the AI performance report the same as PageSpeed Insights or Core Web Vitals?**
No. They are different tools measuring different signals. The AI performance report shows search visibility in AI features; PageSpeed Insights and Core Web Vitals measure load speed and visual stability.

**Does mod_pagespeed improve my AI Overviews visibility?**
No. mod_pagespeed improves how fast and reliably your pages load. It does not control whether AI surfaces cite or show your pages, and it does not change what the AI performance report reports.

**What does mod_pagespeed actually optimize?**
Render-blocking resources, images (WebP/AVIF and resizing), cache lifetimes for static assets, CSS and JavaScript minification, and the LCP-resource subset (preload and deferral). It does not fix INP, and it fixes only the unsized-image case of CLS.

**Do Core Web Vitals still matter for search?**
Yes, separately from this report. Core Web Vitals remain a ranking signal in classic search. The new AI performance report is about visibility in AI features and does not score speed.

---

Google's new report measures whether AI surfaces cite you, not whether your site is fast — two different problems. If the one you have is speed, see how [the Google PageSpeed module](/alternatives/google-pagespeed-module/) and its maintained continuation compare, check [pricing](/pricing/), or run your own audit through [the analyzer](/analyze/).

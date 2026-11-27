---
title: 'Fix INP on WordPress: plugin discipline'
description: 'How to fix INP on WordPress: diagnose the responsible interaction, defer non-critical JS at the server layer, then trim plugin handlers and jQuery.'
date: 2026-04-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'inp', 'wordpress']
draft: false
product: '1.1'
howTo:
  name: How to fix INP on WordPress
  description: Diagnose the slow interaction, use mod_pagespeed to cut JS parse and transfer cost, then trim the plugin handlers and jQuery that actually drive INP, and verify in the field.
  tools:
  - mod_pagespeed
  - nginx
  - Apache
  - Chrome DevTools Performance panel
  - web-vitals (attribution mode)
  - PageSpeed Insights
  - Google Search Console
  - Plugin Performance Profiler (P3)
  steps:
  - name: Find the offending interaction and handler
    text: Identify which click drives INP before changing anything. Record the Chrome DevTools Performance panel while performing real interactions (nav click, search submit, hamburger toggle) and read the Interactions track, or log onINP in web-vitals attribution mode to name the specific script and handler. Cross-reference with PSI field data and WP Site Health.
  - name: Defer and combine plugin JS with mod_pagespeed
    text: Enable defer_javascript, combine_javascript, and rewrite_javascript so script parse and transfer cost no longer collide with the first click. This reduces parse-and-transfer overhead only; the server layer cannot remove third-party event handlers, and defer_javascript must be staged because it can break themes that need jQuery ready synchronously.
  - name: Apply plugin discipline
    text: 'Do the real INP work in the application: profile plugins to find the two or three responsible for most frontend JS and replace or disable them, drop jQuery where the theme allows, move analytics into a tag manager firing after first paint, throttle or disable WP Heartbeat on the frontend, and port admin-ajax handlers to the REST API.'
  - name: Fix handlers mod_pagespeed cannot reach
    text: 'Address the failure modes that live in handler logic, not in JS bytes: defer or remove third-party scripts (chat widgets, off-origin analytics) via tag-manager config, replace plugins that hard-depend on synchronous jQuery init or instrument every click, and rewrite any single slow handler (such as a 90 ms menu toggle) directly, since no server-layer filter can shrink it.'
  - name: Confirm the click is faster in the field
    text: 'Verify the fix actually landed: watch the web-vitals attribution logs for the baseline slow handler to drop, re-record the same interactions in DevTools to confirm the worst INP is under 200 ms, re-run PSI, then wait 28 days and check the Search Console Core Web Vitals report for the URL group to move to Good.'

---

WordPress INP is mostly a plugin-discipline problem in disguise. LCP and CLS clear, then Search Console flags INP on the first nav click or search form — and there are forty plugin handlers that could be the culprit. **mod_pagespeed** (an nginx or Apache module) runs `defer_javascript`, `combine_javascript`, and `rewrite_javascript` to push parse and transfer cost past the first interaction, but the structural fix is removing plugins that attach delegated click listeners to `document`. Both steps below.

This guide is part of our [Core Web Vitals series](/core-web-vitals/).

## What INP measures

Interaction to Next Paint (INP) is the worst-case latency between a user input — click, tap, keypress — and the next frame the browser paints in response. Chrome records every interaction throughout the page's lifetime and reports the 98th-percentile slowest one as that page's INP. Good is **≤ 200 ms**, "needs improvement" goes up to 500 ms, and anything above 500 ms fails. Real-user numbers come from CrUX; the Lighthouse-via-PSI lab number is unreliable because the lab never clicks anything — and on WordPress, where the cost lands on the first real nav or search click, the field number is what to watch. See [web.dev/inp](https://web.dev/articles/inp).

## The most common INP failures on WordPress

1. **Plugin event handlers chain on every click of the nav.** WPML, Polylang, Rank Math, and Yoast attach delegated `click` handlers to `document` that fire on every click — analytics tracking, language detection, breadcrumb logging. The first nav click after page load triggers them all in series, and INP is measured on that exact interaction. We routinely see 250–400 ms recorded latency on a stock multilingual install.
2. **`admin-ajax.php` calls block the main thread on form submit.** Search forms, login forms, and any plugin using `wp_ajax_` hooks send a synchronous `XMLHttpRequest` to `/wp-admin/admin-ajax.php`. The network round-trip itself is fast on a healthy host, but the response JSON parse plus the handler chain on the client adds 80–200 ms to INP on the form-submit interaction. On low-end Android the parse alone can take 60 ms.
3. **jQuery + jQuery Migrate still ship on most WP sites.** Even on WP 6.x with a Gutenberg-only theme, about three out of four plugins force-enqueue `jquery` and bring `jquery-migrate` along. jQuery's first interaction runs ~30 ms of internal init on desktop and ~150 ms on a mid-range Android. That cost is paid the _first_ time the user clicks anything — exactly the worst moment for INP.
4. **WP Heartbeat fires while the user is interacting.** `wp-includes/js/heartbeat.js` polls every 15 s by default. If a heartbeat tick coincides with a user click, the click handler waits behind the tick. INP records the wait.

## Find the offending handler

Before changing anything, identify the responsible interaction. INP is a worst-case metric; you cannot fix it without knowing _which click_.

- Open the page in Chrome, open DevTools → **Performance** panel, click the record button, then perform the interactions a real user would: nav click, search submit, hamburger toggle. Stop. The **Interactions** track lists each input with its INP duration. Click the worst one → the flame chart shows the long-task event handlers responsible.
- Install [`web-vitals`](https://github.com/GoogleChrome/web-vitals) with `attribution` mode in a small `<script type="module">` at the bottom of `footer.php`. Log to console: `onINP(console.log, {reportAllChanges: true})`. The `attribution.eventEntry` field names the script and handler that took the longest.
- Run PSI: `https://pagespeed.web.dev/analysis?url=<URL>`. The "Diagnose performance issues" section lists "Interaction to Next Paint" with field data if there is enough CrUX coverage, lab data otherwise.
- In WP admin: **Tools → Site Health → Info → Performance** lists active plugins. Cross-reference with the worst handler from the Performance panel.

The output you want before continuing is a specific interaction (e.g., "click on `.menu-item a`") and a specific script (e.g., "`wp-content/plugins/wpml/res/js/wpml-tracking.js`").

## Defer plugin JS so the first click doesn't compete with jQuery parse

mod_pagespeed's INP leverage on WordPress is real but partial. The server layer can reduce JS _parse_ and _transfer_ cost; it cannot remove third-party event handlers from the page. Enable, in order:

- **`defer_javascript`** — pushes script execution past the initial render window, so the parse cost no longer collides with the user's first click. Marked **Test first** in the [filters reference](/docs/filters/) because it changes execution order — themes that depend on jQuery being ready synchronously will break. Stage it.
- **`combine_javascript`** — concatenates multiple `<script src>` files into one, removing per-file parser setup. A typical WP install loads 10–20 separate JS files; combining cuts the parser overhead by an order of magnitude.
- **`rewrite_javascript`** — minifies inline and external JS ([safely, without breaking on missing semicolons](/blog/safe-javascript-minification-semicolon-insertion/)). Minification is mostly transfer-size, but on parse-heavy bundles (jQuery + jQuery Migrate + WC fragments) the parse cost scales with byte count, so it is measurable on slower devices.

Minimal nginx config:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters defer_javascript;
pagespeed EnableFilters combine_javascript;
pagespeed EnableFilters rewrite_javascript;
```

After enabling, re-run PSI and re-record the Performance trace from the Diagnose step. INP attribution should move from "long task: jquery.min.js parse" to a smaller handler — that is the signal that the parse-cost dimension is no longer the bottleneck. If attribution stays on a specific plugin's handler after that, MPS has done what it can — the plugin-discipline work below picks up from there.

## Plugin discipline

This is where the real INP wins live on WordPress. Server-layer minification cannot remove a handler that runs 200 ms of analytics logic on every click.

- Run the [Plugin Performance Profiler (P3)](https://wordpress.org/plugins/p3-profiler/) (or its modern fork) to find the two or three plugins responsible for more than half of frontend JS. Replace or disable them. Almost every WP site has at least one offender that does no user-visible work. Chrome's coverage tool tells the same story from the other side — see [finding unused JavaScript with Chrome Coverage](/blog/remove-unused-javascript-chrome-coverage/).
- Drop jQuery if you can. Themes built on Gutenberg blocks (Twenty Twenty-Four onwards) ship jQuery-free; the legacy `jquery` enqueue chain is usually pulled in by a single plugin you do not need.
- Move analytics tags (GA4, Meta Pixel) into Google Tag Manager with a "fire after `pageshow + 3s`" trigger. The first user interaction is then no longer competing with analytics init.
- Throttle WP Heartbeat on the frontend: `add_filter( 'heartbeat_settings', fn($s) => array_merge($s, ['interval' => 60]) );`. Or disable it entirely on the frontend.
- For custom plugins that still post to `admin-ajax.php`, port them to the REST API. REST is roughly 30 % faster cold-path because there is no `admin-init` action chain.
- Audit the search form. If the theme ships an instant-search overlay, it is almost certainly doing more work per keystroke than it needs to.

## How to confirm the click is faster

1. Watch the field-data trend in the `web-vitals` attribution logs (the `onINP` handler from the Diagnose step). PSI summarizes; field logs name the culprit. The slow handler from your baseline should drop or disappear from the top of the list.
2. Open DevTools → Performance, record the same interactions from the Diagnose step, confirm the worst interaction's INP is below 200 ms in the **Interactions** track.
3. Re-run PSI on the same URL. The field "Interaction to Next Paint" should drop below 200 ms (or move from "Poor" to "Needs improvement" if the starting point was very bad).
4. Wait 28 days, then check Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" to "Good". Lab improvements that never show up in CrUX usually mean the worst interaction is one you are not testing in DevTools.

## What to add to your nginx.conf

```nginx
# nginx — minimal config to address INP on WordPress
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
pagespeed RewriteLevel CoreFilters;
pagespeed EnableFilters defer_javascript;
pagespeed EnableFilters combine_javascript;
pagespeed EnableFilters rewrite_javascript;
# Optional, helps the first paint settle before the first interaction:
pagespeed EnableFilters prioritize_critical_css;
```

For Apache:

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed
ModPagespeedRewriteLevel CoreFilters
ModPagespeedEnableFilters defer_javascript
ModPagespeedEnableFilters combine_javascript
ModPagespeedEnableFilters rewrite_javascript
```

## When this doesn't work

Where mod_pagespeed runs out of leverage. The INP failure modes below live in the handler logic itself, not in the JS bytes the rewriter can shrink.

- **Third-party scripts you do not control.** If INP attribution lands on Intercom, HotJar, a chat widget, or a tag-manager-injected analytics script, the rewrite layer cannot help — those scripts are loaded from a different origin and executed in their own scheduler. Defer them via tag-manager config (fire after first paint or after first idle), or remove them.
- **Plugins that hard-depend on synchronous jQuery init.** Some older form plugins, image-gallery plugins, and ecommerce add-ons attach handlers inside `jQuery(document).ready(...)` and assume jQuery is loaded synchronously. `defer_javascript` will break these. Test on staging; if breakage shows up, the plugin needs replacing, not the filter disabling.
- **Plugin-side analytics that run on every click.** A handful of plugins instrument every nav click for their own dashboards. MPS minifies them but cannot remove the work they do. Replace the plugin or disable its tracking option.
- **A 90 ms handler is still a 90 ms handler.** INP is bounded below by the slowest individual handler the user can trigger. If your theme's hamburger-menu toggle does 90 ms of DOM thrashing, no server-layer filter will get you below 90 ms on the click that fires it. Fix the handler.

WordPress INP is mostly a plugin-discipline problem. mod_pagespeed reduces the parse-and-transfer overhead so the underlying handler cost becomes visible — then the work moves to the application.

## Related

- [WordPress full-page caching plugin](/wordpress/)
- [How to fix LCP on WordPress](/blog/fix-lcp-wordpress-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix CLS on WordPress](/blog/fix-cls-wordpress-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix INP on WooCommerce](/blog/fix-inp-woocommerce-2026/)
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full INP guide](/core-web-vitals/inp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as an nginx or Apache module. It optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).

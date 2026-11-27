---
title: 'A WordPress server-side page cache plugin: control plane'
description: 'How a WordPress server-side page cache plugin sets Cache-Control on anonymous pages and purges the mod_pagespeed cache on publish, not caching in PHP.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ["caching", "wordpress", "architecture", "operations", "nginx", "ttfb"]
draft: false
product: '2.0'
---
Hit an anonymous WordPress page through nginx with `curl -I` and read the response headers. You will almost certainly find no `Cache-Control` header at all. WordPress does not mark anonymous pages as publicly cacheable by default. That single fact is why a server-layer page cache sitting in front of WordPress so often does nothing: it sees a response it is not allowed to store, and forwards every request to PHP.

mod_pagespeed reads `Cache-Control` the way the spec says to. On 2.0, the freshness logic honors RFC 9111 directly: `no-store` and `private` make a response uncacheable at MISS time, and `max-age=N` is used as the TTL. There is no custom header protocol to learn. So if WordPress emitted `Cache-Control: public, max-age=600` on an anonymous page, mod_pagespeed would cache that page for ten minutes. It does not, so mod_pagespeed falls back to `pagespeed_html_max_age`, which defaults to 0, which means uncached.

The fix is not more server code. It is teaching WordPress to send the right header, and to tell the cache when to forget. That is the entire job of the WordPress server-side page cache plugin We-Amp publishes on WordPress.org: **WeAmp Cache Control for ModPageSpeed**, now [live in the plugin directory](https://wordpress.org/plugins/weamp-cache-control-for-modpagespeed/). This post is about what it is architecturally, not a feature tour.

## A WordPress server-side page cache plugin that is a control plane, not a cache

The plugin is pure PHP, and it does not cache anything itself. It is a control plane: it sets cache headers on responses and calls a purge API when content changes. The caching happens server-side, in the mod_pagespeed module you already run on nginx or Apache.

That split is deliberate, and it is the same architecture LiteSpeed Cache uses on its own server: the WordPress plugin is a bridge, and the server does the actual storage. We-Amp took LiteSpeed Cache as a benchmark precisely because the model is proven. The difference is the layer underneath. LiteSpeed Cache drives LiteSpeed Web Server. WeAmp Cache Control drives mod_pagespeed, which runs on Apache, nginx and IIS. LiteSpeed Cache is a strong plugin with millions of installs; the difference is simply where the cache lives.

It is also a different layer from the PHP-resident caches like W3 Total Cache and [WP Rocket](/alternatives/wp-rocket/). Those generate cached HTML inside PHP and serve it back through PHP (or via a generated nginx/Apache rule set). They work, and on shared hosting where you cannot install a server module, they are often the only option. WeAmp Cache Control assumes you control your server stack, because mod_pagespeed has to be installed there. That assumption narrows the audience to VPS and dedicated operators, and it is the cost of putting the cache in the server instead of in PHP.

So the plugin has two responsibilities, and they map directly onto the two halves of any cache: writing and invalidating.

## Setting the header: enabling the cache

On the request path, the plugin hooks `wp_headers` and adds `Cache-Control: public, max-age=<ttl>` to anonymous page responses. The TTL is configurable per post type in the admin UI, so your product pages and your blog posts can carry different lifetimes.

The interesting engineering is not adding the header. It is knowing when **not** to. A cached anonymous page served to a logged-in user is a correctness bug, and WordPress core does not protect you from it as reliably as people assume. Core only calls `nocache_headers()` on the frontend when the admin bar is showing. A logged-in subscriber who disabled the admin bar, or any site that calls `show_admin_bar(false)`, gets no protective headers from core at all. So the plugin applies its own checks rather than trusting WordPress to have done it.

`Cache-Control: public` is withheld whenever `is_user_logged_in()`, `is_admin()`, `is_feed()`, `is_search()`, or `is_404()` is true; whenever the request path starts with `/wp-json/`, `/wp-admin/`, `/xmlrpc.php`, or `/wp-login.php`; whenever the response already carries a `Set-Cookie`; and whenever an unsupported `Vary` token is present. Logged-in users, cart cookies, and the other bypass cases instead get `Cache-Control: no-store`, evaluated against a configurable cookie bypass list (defaulting to `wordpress_logged_in_*`, `woocommerce_cart`, `woocommerce_session_*`, `comment_author_*`, and `wp-settings-*`) before any `public` header is even considered.

WooCommerce gets explicit handling because WooCommerce's own headers are not enough. Its `prevent_caching()` emits `no-cache, must-revalidate, max-age=0` (not `no-store`) for cart, checkout, and account pages, and it does not cover product pages for users with items in cart, Blocks-based cart and checkout, or Subscriptions pages. The plugin calls `is_cart()`, `is_checkout()`, and `is_account_page()` itself and emits `no-store` regardless.

There is one server-side prerequisite the plugin cannot do from PHP, and the documentation is blunt about it. mod_pagespeed's cache key is scheme plus hostname plus URL, with no cookie variation. Without an nginx-level bypass, a cached anonymous response can still be served to a logged-in user inside the TTL window, because the cache does not know the cookie changed. So the operator adds a `map` block keyed on `$http_cookie` and wires `proxy_no_cache` / `proxy_cache_bypass` on the module's location. The setup wizard hands you the exact snippet and verifies it is active. This is real activation friction: five to seven steps versus two for a pure-PHP cache. The plugin does not pretend otherwise.

## Purging the header: invalidating the cache

On the write path, the plugin calls the mod_pagespeed purge API when content changes. The primary hook is `transition_post_status`, which fires on the publish transition and on edits to already-published posts, where the narrower `publish_post` would miss the update case. It also hooks `delete_post`, taxonomy and term changes, comment changes, theme switches, nav-menu and widget option updates, and WooCommerce stock changes (which do not run through `transition_post_status` at all). The purge scope is the exact post URL plus the homepage plus the affected archive and feed URLs.

Against 2.0 the call is a `POST` to the worker's `<admin-base>/v1/cache/purge` endpoint with a JSON body naming the absolute URL. The admin base is operator-configured, not a fixed top-level path. Bearer auth is sent only when the worker has an API token configured, so the plugin does not hard-require a key. Every purge target is derived from `get_permalink()` and `home_url()` and normalized — the plugin never accepts URL components from request parameters, and it refuses wildcard values outright as a contract-wide guard. On 2.0 there is no wildcard purge at all: a single-URL purge is exact-match, and a full flush takes a separate, explicitly confirmed request. The refusal still matters because on 1.15 a trailing `*` flushes the entire cache, so the plugin never sends one against either line. For deeper context on why one purge touches more than one cache entry, see [What Happens When You Purge One URL](/blog/single-url-cache-purge-optimizing-proxy/).

The 1.15 line needs a clear caveat. **mod_pagespeed 1.15 does not cache HTML pages.** It is an in-process streaming rewriter; its cache holds optimized images, CSS, and JavaScript, not full pages. Worse, with the default `ModifyCachingHeaders on`, 1.15 will overwrite the plugin's `Cache-Control: public` on rewritten HTML with `max-age=0, no-cache`. So on 1.15 the plugin is a purge-only control plane: its purge calls evict optimized sub-resources, not pages. If you want full-page caching on 1.15, you put an external cache in front (nginx `proxy_cache`, Varnish, or a CDN) and set `ModifyCachingHeaders off` so the plugin's headers reach that front cache. That is documented as an advanced recipe, not a default claim. On 2.0, the interceptor caches HTML itself and no front cache is required.

The plugin auto-detects which one of the two is running and adjusts. The same setup wizard and purge contract cover both, which is the point of putting the control plane in WordPress and the cache in the server.

## Related

- [What Happens When You Purge One URL](/blog/single-url-cache-purge-optimizing-proxy/) — the cache fan-out a single purge call actually triggers
- [Where TTFB Actually Goes, and the Server-Layer Fix](/blog/reduce-ttfb-server-layer-2026/) — why a server-side page cache is the lever for time-to-first-byte
- [Fixing LCP on WordPress](/blog/fix-lcp-wordpress-2026/) — what server-layer caching and optimization do for Largest Contentful Paint
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [Fixing CLS on WordPress](/blog/fix-cls-wordpress-2026/) — the layout-shift fixes that pair with a cacheable anonymous page
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [mod_pagespeed vs WP Rocket](/alternatives/wp-rocket/) — where the cache lives, PHP layer versus server module
- [Running mod_pagespeed with Docker Compose](/blog/run-with-docker-compose/) — standing up the optimizer worker the plugin talks to
- [Cache-Control and purging](/docs/cache-control/) — how full-page caching and invalidation work in the module
- [The WordPress plugin landing page](/wordpress/) — install steps, the 1.15-vs-2.0 difference, and the FAQ

If you already run mod_pagespeed on a VPS or dedicated box, the missing piece is usually not optimization — it is that anonymous pages never get marked cacheable, so the cache never engages. The plugin closes that gap from inside WordPress without a line of server code beyond one nginx `map` block. It is free and GPL-2.0, and it is [live on WordPress.org](https://wordpress.org/plugins/weamp-cache-control-for-modpagespeed/). The server module it drives is licensed under Apache-2.0 and free to run in development and in production. Start with the module via the [install guide](/download/), then read the [Cache-Control behavior](/docs/cache-control/) so you know exactly what the plugin's headers will do once they arrive.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

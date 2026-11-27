---
title: 'Set Cache-Control headers'
description: 'Which Cache-Control headers to set on your origin, by content type and by framework, for correct caching with mod_pagespeed.'
order: 21
group: 'Configure'
lastUpdated: 2026-10-22
---

:::note[Since 2.0]
The recommendations on this page rely on conditional revalidation, which the
2.0 line introduced. Without it, `must-revalidate` causes full origin
re-fetches on every stale request. Upgrade before following them.
:::

Your origin's `Cache-Control` headers directly control how mod_pagespeed caches
and serves your content. Getting these right prevents stale content and cuts
unnecessary origin traffic.

When your origin sends no `Cache-Control` header, mod_pagespeed applies
configurable defaults (HTML: `no-cache`, CSS/JS: 300s, images: 1800s in
[safe mode](/docs/cache-modes/)) and logs
a warning. Explicit headers are better than relying on
[these fallback TTLs](/blog/default-cache-ttl-heuristic-freshness/).

## Recommended headers by content type

### HTML pages

```http
Cache-Control: public, max-age=60, must-revalidate
ETag: "content-hash-or-version"
```

HTML references hashed assets. A deploy changes HTML content to point at new
asset URLs. Short `max-age=60` provides one minute of caching. With conditional
revalidation, stale requests are cheap — origin returns 304 when content hasn't
changed.

Use `max-age=0` only when absolute real-time freshness is required (stock
tickers, live scores). Every request with `max-age=0` triggers a conditional
revalidation, which means an origin round-trip even when content hasn't changed.

### Hashed static assets (CSS/JS with fingerprints)

```http
Cache-Control: public, max-age=31536000, immutable
```

The URL changes on every build. Content at a given URL never changes.
mod_pagespeed strips `immutable` from its transformed output — the optimized
bytes depend on mutable config and software version — but preserves the long
TTL, capping max-age at `pagespeed_immutable_max_age` (default: 7 days).

### Non-hashed static assets

```http
Cache-Control: public, max-age=3600, must-revalidate
ETag: "file-mtime-and-size"
```

The URL is stable but content may change. Short max-age with `must-revalidate`
keeps content fresh. Conditional revalidation makes the revalidation cheap.

### User-uploaded images

```http
Cache-Control: public, max-age=86400
```

User images rarely change at the same URL. 24-hour caching is reasonable.
If images can be replaced at the same URL, add `must-revalidate` and an `ETag`.

### API responses and dynamic content

```http
Cache-Control: no-store
```

mod_pagespeed does not cache `no-store` responses. The response passes through
unchanged.

## Framework configuration

| Framework          | Where to Set Headers                | HTML                          | Hashed Assets                 |
| ------------------ | ----------------------------------- | ----------------------------- | ----------------------------- |
| **nginx** (static) | `nginx.conf` per `location`         | `max-age=60, must-revalidate` | `max-age=31536000, immutable` |
| **Apache**         | `.htaccess` `Header set`            | `max-age=60, must-revalidate` | `max-age=31536000, immutable` |
| **Next.js 13+**    | `next.config.js` `headers()`        | `max-age=60, must-revalidate` | Automatic for `_next/static/` |
| **Astro 4+**       | Server adapter `headers`            | `max-age=60, must-revalidate` | Automatic via content hashing |
| **Rails 7+**       | `config.public_file_server.headers` | `max-age=60, must-revalidate` | Automatic via Propshaft       |
| **WordPress**      | Cache plugin or `.htaccess`         | `max-age=60, must-revalidate` | Theme-dependent               |

## PURGE vs natural expiration

`PURGE` immediately invalidates all cached variants for a URL. Use it for
urgent content corrections. For routine deploys, let `must-revalidate` handle
freshness — conditional revalidation is cheaper than purge-and-rebuild
because it preserves optimized image variants (AVIF, WebP) when only the HTML
changed. See [304 conditional revalidation vs PURGE](/blog/conditional-revalidation-304-vs-active-purge/)
for when each is the right invalidation strategy.

## Cache mode

The `pagespeed_cache_mode` directive controls how mod_pagespeed assembles
Cache-Control headers on optimized responses. Safe mode (the default) adds
`must-revalidate` and uses short TTLs for quick recovery from
misconfigurations. Aggressive mode uses long TTLs with `public` for maximum
cache efficiency. See [Cache Modes](/docs/cache-modes/) for full details, or
[the safety math behind must-revalidate vs aggressive TTLs](/blog/cache-mode-safety-must-revalidate-vs-aggressive/)
for how each mode trades freshness against origin load.

## mod_pagespeed directives {#modpagespeed-directives}

These directives control the default max-age values applied when origin
responses lack a `Cache-Control` header. They do not override explicit
origin headers.

```nginx
pagespeed_html_max_age 0;      # Default for HTML (seconds, default: 0 = no-cache)
pagespeed_css_max_age 300;     # Default for CSS/JS (seconds, default: 300)
pagespeed_image_max_age 1800;  # Default for images (seconds, safe: 1800, aggressive: 86400)
```

When `pagespeed_html_max_age` is 0 and the origin sends no `Cache-Control`,
HTML responses are served with `Cache-Control: no-cache` — the client must
revalidate on every request.

### Conditional revalidation

```nginx
pagespeed_conditional_revalidation on;   # default: on
```

When enabled, stale cache entries with stored `ETag` or `Last-Modified` values
trigger conditional requests (`If-None-Match` / `If-Modified-Since`) instead of
full re-fetches. On 304, the cached content is refreshed without re-downloading
or re-optimizing. Disable only if your origin mishandles conditional requests.

### Browser force-refresh

```nginx
pagespeed_force_refresh_html on;   # default: on
pagespeed_force_refresh off;       # default: off
```

When a user performs a force-refresh (Ctrl+F5 or Shift+Reload), the browser sends
`Cache-Control: no-cache` or `Pragma: no-cache`. mod_pagespeed detects these signals
and forces revalidation against your origin, even if the cached entry is fresh.

For HTML, this is enabled by default — users expect force-refresh to fetch the latest
page. For images, CSS, and JS, it is off by default to prevent unnecessary origin
load. Enable `pagespeed_force_refresh` if you want force-refresh to apply to all
content types.

When conditional revalidation is also enabled (the default), force-refresh uses
`If-None-Match` / `If-Modified-Since` to avoid re-downloading unchanged content.

## Cache-extension filters

The filters in this section belong to the in-process module on Apache,
nginx and IIS. They extend browser cache lifetimes and manage resource URLs
across domains. Browsers cache resources for a year, but pick up updated
content the moment a resource changes, because the URL carries a content
hash. The `extend_cache` CoreFilter drives this: it sets a 1-year cache
lifetime on CSS, JS, and image URLs.

### extend_cache {#extend_cache}

[Full guide →](/docs/filters/extend_cache/)

**Core filter.** `extend_cache` rewrites the URLs of stylesheets, scripts, and images to carry a content hash, giving each resource a `.pagespeed.ce.` URL, and serves those resources with a one-year `Cache-Control: max-age`. When the original file changes, its hash changes too: once the module's cached copy of the original expires (origin lifetime, or `ImplicitCacheTtlMs` when none is set), the page points at a new URL. Live demo: [extend_cache](/examples/extend_cache/).

```html
<!-- before -->
<link rel="stylesheet" href="/css/site.css" />

<!-- after: served with Cache-Control: max-age=31536000 -->
<link rel="stylesheet" href="/css/site.css.pagespeed.ce.HASH.css" />
```

#### When it helps and when it does not

It helps wherever the origin cannot set long cache lifetimes itself, which is common on shared hosting and legacy applications: first visits cache every static resource for a year, and repeat visits stop revalidating them. It does nothing for resources that already carry a fingerprint and a long max-age from the build pipeline, since those URLs are already immutable in practice. It also does not make HTML cacheable: the page itself keeps its own short lifetime, because the page is what hands out the new hashed URLs after a deploy.

#### How it decides

Only resources on domains the module is authorized to rewrite are candidates, and resources already rewritten by another filter (a minified stylesheet, an optimized image) already carry a content hash, so there is nothing to extend. The one-year lifetime works because the URL is derived from the bytes: changed content gets a new URL rather than new bytes at the old one, so visitors pick it up once they load a page that references the new URL. The name is compound: it switches on `extend_cache_css`, `extend_cache_images`, and `extend_cache_scripts`, and each member can be disabled on its own.

#### Risks

- HTML and the resources it references must share one configuration; a setup where two virtual hosts disagree on the same file can serve a hash that does not match. See [Virtual hosts](/docs/configuration/#virtual-hosts).
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-extend_cache` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters extend_cache
```

```nginx
# Nginx
pagespeed EnableFilters extend_cache;
```

The sub-filters `extend_cache_css`, `extend_cache_images`, and `extend_cache_scripts` are included when you enable `extend_cache`, which turns on all three. Each can also be enabled individually with `EnableFilters` (for example, `extend_cache_images` alone).

### extend_cache_pdfs {#extend_cache_pdfs}

[Full guide →](/docs/filters/extend_cache_pdfs/)

**Not a CoreFilter.** Applies the same content-hash-based cache extension to PDF file links. Enable this filter if your site serves PDFs that change infrequently.

```apache
# Apache
ModPagespeedEnableFilters extend_cache_pdfs
```

```nginx
# Nginx
pagespeed EnableFilters extend_cache_pdfs;
```

### rewrite_domains {#rewrite_domains}

[Full guide →](/docs/filters/rewrite_domains/)

**Not a CoreFilter. Test before deploying.** Rewrites resource URLs to use domains specified by `MapRewriteDomain` or `ShardDomain` directives. Useful for CDN integration or domain sharding.

```apache
# Apache
ModPagespeedEnableFilters rewrite_domains
```

```nginx
# Nginx
pagespeed EnableFilters rewrite_domains;
```

This filter only affects resources that mod_pagespeed does not otherwise optimize. Resources already rewritten by other filters ([`rewrite_css`](/docs/css-filters/#rewrite_css), [`rewrite_images`](/docs/image-filters/#rewrite_images), etc.) already have their domains set by those filters.

See [Domain configuration](/docs/domain-configuration/) for `MapRewriteDomain` and `ShardDomain` setup.

### local_storage_cache {#local_storage_cache}

[Full guide →](/docs/filters/local_storage_cache/)

**Retired from 2.7.0.** From 2.7.0, `local_storage_cache` is accepted and ignored: the name still works in every configuration syntax, in `EnableFilters` and in query parameters, enabling it logs one warning per process, and nothing changes in the page. Disabling or forbidding it is silent, so a configuration that still lists it keeps loading; remove the line when convenient. A rewritten page carries no local-storage cookie, script or `data-pagespeed-lsc-*` attribute, and the filter no longer adds a `Vary: User-Agent` token or a `noscript` redirect.

The filter moved the bytes of inlined images and CSS into the browser's `localStorage` and tracked them in a `_GPSLSC` cookie. That cookie grew with every stored resource until a server could start refusing requests with "400 request header too large", and `localStorage` has size limits (typically 5-10 MB per origin) and can be cleared by the user at any time. In 2.6.0 and earlier the filter still runs as described here; it is not recommended there either.

For the same repeat-view saving, use [`extend_cache`](#extend_cache): it keeps resources at external, content-hashed URLs with a one-year lifetime, with no cookie and no scripts.

## Auditing your origin

The [web console](https://we-amp.com/console/) flags URLs where the origin sends no `Cache-Control`
header. Use the URL inspector to check freshness status and identify origins that
need explicit headers.

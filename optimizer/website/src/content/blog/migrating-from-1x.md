---
title: 'Migrate from mod_pagespeed 1.13 or ngx_pagespeed: 2.0 guide'
description: 'Migrating from the archived mod_pagespeed 1.13.x or ngx_pagespeed: directive mapping, image-format checks, a verification checklist and the upgrade path.'
date: 2026-02-10
author: 'Otto van der Schaaf'
tags: ['migration', 'guide', 'nginx']
draft: false
lastUpdated: 2026-10-22
product: '2.0'
faq:
  - q: "Can I run the archived 1.13.x and 2.0 side by side?"
    a: "Yes. Deploy them on different servers or different nginx server blocks. They share no state, so there is no conflict."
  - q: "Do I need to change my HTML or application code?"
    a: "No. All optimization is transparent at the reverse-proxy level. Your application serves the same responses it always has."
  - q: "Will my existing cache be preserved?"
    a: "No. The optimizer worker uses Cyclone, a different cache format from the 1.13.x file cache. The cache will be cold on first start and warm up as traffic flows through."
---

> Written for the ModPageSpeed 2.0 line: the Docker Compose setup and the `pagespeed_*` directives below are 2.0's. On mod_pagespeed 2 a 1.13 or ngx_pagespeed configuration carries over to the native module; see [migrate from the archived Google module](/docs/migrate-from-google-mod-pagespeed/) and [migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/).

## What changed and why

The original mod_pagespeed 1.13.x was built around the RewriteDriver -- over 2,000 lines of orchestration code that managed a pipeline of 60+ filters, each modifying the HTML response in sequence during the request. Filters had strict ordering dependencies: `combine_css` ran before `rewrite_css`, which ran before `inline_css`, and so on. Adding a filter meant reasoning about how it interacted with every existing one.

The architecture was designed for Apache. It hooked into Apache's output filter chain, and the nginx port (`ngx_pagespeed`) adapted the filter pipeline to nginx's event-driven model. This translation layer added significant maintenance overhead, particularly around subrequest handling and connection lifecycle management.

ModPageSpeed 2.0 takes a different architectural approach. The underlying optimization libraries -- the HTML parser, CSS minifier, JS minifier, and image codecs -- are individually well-tested and capable (and form the foundation of 2.0). The key change is moving orchestration out of the request path entirely, into a separate worker that optimizes content asynchronously.

2.0 is a ground-up rethink: it keeps the proven PSOL optimization libraries and replaces the architecture around them. If you are running the archived mod_pagespeed 1.13.x today, this guide will walk you through what changed and how to migrate.

## Architecture comparison

The original mod_pagespeed 1.13.x processed everything synchronously within the request:

```
Client -> Apache/Nginx -> mod_pagespeed filters (sync) -> Origin
                          [RewriteDriver orchestration]
                          [60+ filters in pipeline]
                          [Rewrite cache for sub-resources]
```

The 2.0 re-architecture introduced a three-component architecture where optimization happens outside the request path:

```
Client -> Nginx interceptor -> Cyclone Cache (mmap) -> Client
               |                    ^
               | (fire-and-forget)  | (write variant)
               v                    |
          The worker     -----------+
          [HTML: critical CSS injection]
          [CSS: four-phase minification]
          [JS: token-aware minification]
          [Images: WebP/AVIF/optimized-original]
```

The differences are significant:

**Synchronous vs. asynchronous.** In the original 1.13.x, every response was transformed in-flight, adding latency to every request. In 2.0, the first request gets the original response (served from cache with `X-PageSpeed: MISS`), and the worker optimizes it in the background. Subsequent requests get the optimized variant (`X-PageSpeed: HIT`) with zero processing overhead -- just a memory-mapped cache read.

**Single-process vs. multi-process.** In the original 1.13.x, the optimization code ran inside the web server process. In 2.0, the nginx interceptor is a thin cache layer, and the worker is a separate process. They share a single Cyclone cache file with memory-mapped directories (`enable_mmap_directory = true`), so writes from either process are immediately visible to the other.

**Filter pipeline vs. content-type dispatch.** Instead of 60+ filters with ordering dependencies, the worker dispatches on content type: HTML, CSS, JavaScript, or Image. Each path runs independently. There is no filter interaction to reason about.

## Configuration mapping

Many of the original mod_pagespeed 1.13.x directives have no direct equivalent because 2.0 applies optimizations automatically based on content type. Here is a mapping of the most commonly used directives:

| 1.13.x Directive                                     | 2.0 Equivalent                 | Notes                                                                                                             |
| ---------------------------------------------------- | ------------------------------ | ----------------------------------------------------------------------------------------------------------------- |
| `ModPagespeedEnableFilters`                          | Automatic                      | All optimizations enabled by default                                                                              |
| `ModPagespeedDisableFilters rewrite_images`          | `--disable-image`              | Worker flag                                                                                                       |
| `ModPagespeedDisableFilters rewrite_css`             | `--disable-css`                | Worker flag                                                                                                       |
| `ModPagespeedDisableFilters rewrite_javascript`      | `--disable-js`                 | Worker flag                                                                                                       |
| `ModPagespeedDisableFilters prioritize_critical_css` | `--disable-html`               | Worker flag                                                                                                       |
| `ModPagespeedDisallow /pattern`                      | `pagespeed_disallow /pattern;` | nginx directive                                                                                                   |
| `ModPagespeedCacheSizeMb`                            | `--cache-size`                 | Worker flag — value is now in **bytes**, not MB; multiply by 1048576 (e.g. `1024` MB → `--cache-size 1073741824`) |
| `ModPagespeedFileCachePath`                          | `pagespeed_cache_path`         | nginx directive                                                                                                   |

**Deliberately omitted features.** Several classic filters have no 2.0 equivalent because the 2.0 architecture makes them unnecessary or handles their use case differently. The [mod_pagespeed module](/) keeps these filters if you need them:

- `combine_css` / `combine_javascript` -- Not needed. HTTP/2 multiplexing eliminates the round-trip cost of multiple small files.
- `lazyload_images` -- Replaced with native `loading="lazy"` attribute injection. The worker's HTML transform pipeline automatically adds `loading="lazy"` to `<img>` and `<iframe>` tags, with the LCP candidate image (or first body image as fallback) receiving `fetchpriority="high"` instead. This is enabled by default. The classic `lazyload_images` filter used a JavaScript-based approach; 2.0 uses the browser-native attribute.
- `defer_javascript` -- Not implemented. The `defer` and `async` attributes on `<script>` tags are the standard solution.
- `inline_css` / `inline_javascript` -- Replaced by critical CSS injection, which is more targeted (only above-the-fold rules).
- `sprite_images` -- Not implemented. HTTP/2 and modern image formats make spriting counterproductive.

## Step-by-step migration

### 1. Back up your existing configuration

Save your current mod_pagespeed configuration before making any changes:

```bash
# Apache
cp /etc/apache2/mods-enabled/pagespeed.conf ~/pagespeed-1x-backup.conf

# Nginx (ngx_pagespeed)
cp /etc/nginx/nginx.conf ~/nginx-1x-backup.conf
```

### 2. Install mod_pagespeed

Deploy the three components using Docker Compose or systemd. The Docker Compose approach is recommended for initial testing:

```yaml
# docker-compose.yml (simplified)
services:
  worker:
    image: ghcr.io/we-amp/pagespeed-worker:2.8.1
    command: >
      /usr/bin/factory_worker
      --socket /shared/pagespeed.sock
      --cache-path /shared/cache.vol
      --cache-size 1073741824
    volumes:
      - shared:/shared

  nginx:
    image: ghcr.io/we-amp/pagespeed-nginx:2.8.1
    volumes:
      - shared:/shared
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
    ports:
      - '80:80'

volumes:
  shared:
```

### 3. Configure nginx

Create your nginx configuration with the PageSpeed module:

```nginx
load_module /usr/lib/nginx/modules/ngx_pagespeed_module.so;

server {
    listen 80;
    server_name example.com;

    pagespeed on;
    pagespeed_cache_path /shared/cache.vol;
    # Worker socket and HTML toggle are read
    # automatically from pagespeed-shared.conf (written by the worker)

    # Migrate your Disallow patterns
    pagespeed_disallow /api/;
    pagespeed_disallow /admin/;
    pagespeed_disallow "*.woff2";

    location / {
        proxy_pass http://your-backend:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

### 4. Start the services and verify

```bash
docker compose up -d

# Check the worker health endpoint
socat - UNIX-CONNECT:/shared/pagespeed.sock.health
# Expected: OK 0/128 notifs=0 variants=0 proactive=0 errors=0 cache_entries=0

# Make a test request
curl -I http://localhost/
# Look for: X-PageSpeed: MISS (first request)

# Wait a moment, then request again
curl -I http://localhost/
# Look for: X-PageSpeed: HIT (optimized variant served)
```

## Testing your migration

Validate the migration before you route production traffic through it.

**Check response headers.** Every response through mod_pagespeed includes an `X-PageSpeed` header with either `MISS` (original content) or `HIT` (optimized variant). After warming the cache, verify that repeated requests return `HIT`.

**Verify image format negotiation.** The worker picks [AVIF, WebP, or the original](/blog/avif-vs-webp-2026/) from each request's `Accept` header. Send requests with different Accept headers and confirm you receive the correct format:

```bash
# Request AVIF
curl -H "Accept: image/avif,image/webp,*/*" -o /dev/null -w "%{content_type}" http://localhost/image.jpg
# Expected: image/avif

# Request WebP
curl -H "Accept: image/webp,*/*" -o /dev/null -w "%{content_type}" http://localhost/image.jpg
# Expected: image/webp

# Request original
curl -H "Accept: image/jpeg" -o /dev/null -w "%{content_type}" http://localhost/image.jpg
# Expected: image/jpeg
```

`Accept: */*` (curl's default) and a request with no `Accept` header get WebP, not the original: a wildcard says the client takes any image type. See [Image format and the Accept header](/docs/cdn-integration/#image-format-negotiation).

**Confirm CSS and JS minification.** Compare content lengths between the origin and the optimized responses. The worker only writes a variant if minification actually reduced the size, so the optimized version should always be smaller or equal.

**Monitor the health endpoint.** The worker exposes statistics through its health check socket, reporting active connections, notifications received, variants written, and error counts. Set up monitoring on these counters to track optimization progress and catch issues early.

**Gradual rollout.** Start with a single backend server or a canary server block in nginx. Route a fraction of traffic through mod_pagespeed, compare Core Web Vitals in your RUM data, and expand once you are confident the optimization is correct.

## FAQ

**Can I run the archived 1.13.x and 2.0 side by side?** Yes. Deploy them on different servers or different nginx server blocks. They share no state, so there is no conflict.

**Will my existing cache be preserved?** No. The optimizer worker uses Cyclone, a different cache format from the 1.13.x file cache. The cache will be cold on first start and warm up as traffic flows through.

**Do I need to change my HTML or application code?** No. All optimization is transparent at the reverse-proxy level. Your application serves the same responses it always has.

**What about Apache support?** The reverse-proxy deployment uses nginx internally as its caching proxy, but it deploys in front of any HTTP origin server, including Apache. Run the Docker Compose setup with your Apache server as the backend origin. If you want a native Apache module rather than a reverse proxy, [the module](/mod-pagespeed-still-maintained/) is the in-process part of the same product: same directives, current toolchain.

**What happened to specific filters?** The configuration mapping table above covers the major filters. In general, filters that work around HTTP/1.1 limitations (combining, spriting, inlining) have been intentionally dropped because HTTP/2 makes them unnecessary or counterproductive. Filters that perform genuine optimization (image transcoding, CSS/JS minification, critical CSS) are built into the worker's content-type dispatch.

## Related

- [mod_pagespeed: install and getting started](/docs/getting-started/)
- [What mod_pagespeed optimizes](/features/)
- [mod_pagespeed — the native Apache, nginx and IIS module](/)
- [Migrate to mod_pagespeed](/mod-pagespeed-still-maintained/)
- [Why I rebuilt mod_pagespeed](/blog/why-i-rebuilt-mod-pagespeed/)
- [Run ModPageSpeed 2.0 with Docker Compose](/blog/run-with-docker-compose/)

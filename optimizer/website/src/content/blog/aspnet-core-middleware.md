---
title: 'mod_pagespeed for ASP.NET Core: middleware via NuGet'
description: 'Run the mod_pagespeed optimization pipeline as ASP.NET Core middleware from NuGet: image transcoding, critical CSS and cache serving in 2 lines of C#.'
date: 2026-02-12
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['architecture', 'announcement', 'aspnet-core']
product: '2.0'
draft: false
---

The optimizer worker has been nginx-only until now. The nginx interceptor does zero-copy cache serving, 103 Early Hints, and sub-millisecond variant selection — but it requires nginx.

If your application runs on ASP.NET Core, adding nginx as a reverse proxy just to get page optimization is a significant architectural change. You inherit nginx's configuration language, its process model, and its deployment story.

The ASP.NET Core middleware is a second integration path: same C++ optimization pipeline, called from your app through a NuGet package.

## Install

Two packages — the middleware itself, and the native assets for your runtime.

```shell
dotnet add package WeAmp.PageSpeed.AspNetCore
dotnet add package WeAmp.PageSpeed.NativeAssets.Linux      # linux-x64
# or: dotnet add package WeAmp.PageSpeed.NativeAssets.macOS   # osx-arm64
# or: dotnet add package WeAmp.PageSpeed.NativeAssets.Windows # win-x64
```

The managed packages are platform-independent. The NativeAssets package bundles `libpagespeed` and the `factory_worker` binary for one runtime identifier. Pick the one matching your deployment target, or reference more than one if you ship cross-platform.

## Wire it up

A minimal `Program.cs`:

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = "/data/cache.vol";
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;
    options.Worker.SocketPath = "/data/pagespeed.sock";
});

var app = builder.Build();

app.UsePageSpeed();
app.UseStaticFiles();
app.UseRouting();

app.Run();
```

`AddPageSpeed()` also takes an `IConfiguration` overload that binds to a `PageSpeed` section in `appsettings.json` — see [ASP.NET Core Getting Started](/docs/aspnet-getting-started/) for the JSON layout. Options hot-reload through `IOptionsMonitor<PageSpeedOptions>`.

## Standalone vs full stack

The middleware supports two deployment modes.

**Standalone** — no worker, no image optimization. The middleware processes HTML only: critical CSS inlining, lazy-load attributes, explicit image dimensions, LCP preload hints, preconnect hints for third-party origins. Set `Worker.SocketPath` to `null` (or omit it) and the middleware operates independently. Useful when image optimization already lives elsewhere — a CDN, a build step — and you just want HTML processing.

**Full stack** — ASP.NET middleware plus worker. The middleware does HTML processing and cache serving. The worker transcodes images (WebP, AVIF, SVG), [resizes by viewport](/blog/viewport-aware-image-optimization/), writes [Save-Data quality variants](/blog/save-data-bandwidth/), and pre-compresses alternates (gzip, brotli). Same optimization pipeline as the nginx integration, with the same cache format and the same variant selection.

For the full stack the worker runs alongside your ASP.NET app and shares a Cyclone cache file over a volume — the same colocated layout the [production deployment guide](/docs/production-deployment/) covers for the nginx path:

```yaml
services:
  worker:
    build:
      target: worker-runtime
    volumes:
      - shared:/shared

  aspnet:
    build:
      target: aspnet-runtime
    environment:
      PAGESPEED_CACHE_PATH: /shared/cache.vol
      PAGESPEED_SOCKET_PATH: /shared/pagespeed.sock
    volumes:
      - shared:/shared
    depends_on:
      - worker

volumes:
  shared:
```

Both processes open the cache file via [memory-mapped I/O](/blog/memory-mapped-cache-zero-copy-serving/). The middleware writes originals, the worker writes optimized variants, and each side sees the other's writes immediately.

```bash
cd samples/aspnetcore/samples/DemoSite
./run-demo.sh               # Full stack: ASP.NET + worker
./run-demo.sh --standalone  # HTML processing only, no worker
```

## How a request flows

1. **Classify.** The middleware reads `Accept`, `User-Agent`, `Save-Data`, and `Accept-Encoding` and calls into the native library to produce a 32-bit capability mask. Same classification as the nginx interceptor.

2. **Cache lookup.** `ReadBest(url, hostname, scheme, mask)` on the shared Cyclone cache returns the best matching variant, or nothing.

3. **HIT.** The cached content is served directly. The middleware reads any stored early hints (preload URLs, preconnect origins) and adds them as `Link` response headers. Response gets `X-PageSpeed: HIT`.

4. **MISS.** The middleware lets the request proceed through the rest of the pipeline. It captures the response body, checks that it is HTML within size limits, and runs it through the native HTML processor. The processor extracts [critical CSS](/blog/critical-css-heuristics/), adds lazy-load attributes, injects image dimensions, and collects preload hints. The processed HTML and the early hints are written to the cache, and the worker is notified asynchronously to generate image variants and compressed alternates.

5. **Worker notification.** A bounded channel queues notifications without blocking the request path. The background service drains the channel and sends each notification to the worker over a Unix socket. In standalone mode the channel drains into a no-op.

## Nginx vs middleware

|                        | Nginx interceptor                | ASP.NET middleware                 |
| ---------------------- | -------------------------------- | ---------------------------------- |
| **Cache serving**      | Zero-copy via mmap'd `ngx_buf_t` | Memory copy into response stream   |
| **103 Early Hints**    | Sent before proxying on MISS     | Not supported (Kestrel limitation) |
| **Integration**        | Reverse proxy in front of origin | NuGet package in your pipeline     |
| **Configuration**      | nginx.conf directives            | C# options / appsettings.json      |
| **Deployment**         | Separate nginx process           | Same process as your app           |
| **HTML processing**    | Same pipeline                    | Same pipeline                      |
| **Image optimization** | Same worker                      | Same worker                        |
| **Cache format**       | Same Cyclone cache               | Same Cyclone cache                 |
| **Variant selection**  | Same 32-bit mask                 | Same 32-bit mask                   |
| **Recompilation**      | Requires nginx headers           | `dotnet add package`               |

Nginx still serves cache hits faster. Zero-copy mmap means cached variants go from disk to socket with no intermediate buffer; the middleware copies through the managed-memory pipeline. The nginx interceptor also sends 103 Early Hints on cache misses — Kestrel doesn't expose that to middleware yet.

The middleware's trade is integration. A NuGet package, two lines of C#, configuration in `appsettings.json`. No separate process, no `nginx.conf`, no reverse proxy.

Both paths share the optimization pipeline, the cache, the worker, and the variant logic. Only the serving layer differs.

## Other languages

The C API is plain. `libpagespeed.so` exposes a stable set of functions — classify a request, read from cache, process HTML, notify the worker — that anything with FFI can call. ASP.NET Core is the first non-nginx integration, not the last; a Go middleware, a Rust middleware, or a Python WSGI wrapper would call the same functions.

---

Once it's running, append `?bypass=1` to any URL to compare the optimized output against the original.

_For current installation instructions and configuration reference, see [ASP.NET Core Getting Started](/docs/aspnet-getting-started/)._

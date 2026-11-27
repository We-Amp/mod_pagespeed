---
title: 'ASP.NET Core image optimization in C#: WebP and AVIF'
description: 'Serve WebP and AVIF from ASP.NET Core with no controller or Razor changes: WeAmp.PageSpeed middleware negotiates on Accept, sizes images, preloads LCP.'
date: 2026-05-20
lastUpdated: 2026-10-20
author: 'Otto van der Schaaf'
tags: ['aspnet-core', 'image-optimization', 'webp', 'avif', 'dotnet']
draft: false
product: '2.0'
---

The standard ASP.NET Core image story is: build-time transcode with
ImageSharp, hand-write the `<picture>` tags, hope your build pipeline keeps
up with new content. The
[WeAmp.PageSpeed.AspNetCore](/go/nuget/?from=blog-aspnet-image-lead)
middleware takes a different approach: runtime content negotiation against
the client's `Accept` header, with the transcoded variants generated
asynchronously by a background service and cached in a memory-mapped Cyclone
volume.

If you've read [ModPageSpeed 2.0 now works with ASP.NET
Core](/blog/aspnet-core-middleware/), that piece covered the architecture.
This one is the image story specifically: how `<img src="hero.jpg">`
becomes WebP for Chrome, AVIF for modern Safari, and a viewport-sized
variant for the user's actual screen, without changing a single Razor
view.

## Two lines

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddPageSpeed();

var app = builder.Build();
app.UsePageSpeed();        // before UseStaticFiles / UseResponseCompression
app.MapRazorPages();
app.Run();
```

`AddPageSpeed()` registers the cache, the HTML processor, and a background
notification channel. `UsePageSpeed()` inserts the middleware in the
request pipeline. The order matters: PageSpeed needs to see responses
before `UseResponseCompression` recompresses them, and before
`UseStaticFiles` short-circuits the pipeline for static asset requests.

Install is a single package. Native binaries for `linux-x64`,
`linux-arm64`, `osx-arm64`, and `win-x64` come in transitively via the
matching `WeAmp.PageSpeed.NativeAssets.*` package:

```shell
$ dotnet add package WeAmp.PageSpeed.AspNetCore
```

That's it. No `appsettings.json` changes are required for image
optimization; the defaults work. For the full optimization pipeline
(image transcoding included), run the worker as a sidecar. See the
[Getting Started guide](/docs/aspnet-getting-started/) for the Docker
Compose file.

## What the middleware does to a request

Take a vanilla `<img src="/img/hero.jpg">`. Client is Chrome 120, viewport
1280px wide, `Accept: image/avif, image/webp, image/apng, image/*, */*`.

Request flow:

1. Razor renders the page. PageSpeed buffers the HTML response (the
   middleware sits before `UseResponseCompression`).
2. The HTML processor scans for `<img>` tags. For each one, it rewrites
   the `src` to a versioned URL like
   `/img/hero.jpg.pagespeed.ic.7a3f9e2c.webp` and adds explicit `width`
   and `height` attributes if it can determine them from the cache.
3. On the next request for that rewritten URL, the cache lookup returns
   the pre-generated WebP variant. Zero application code involved; the
   middleware serves the bytes directly with the correct `Content-Type`
   and `Vary: Accept, Save-Data, User-Agent` headers.
4. If the variant doesn't exist yet (first request after deploy), the
   middleware returns the original JPEG and asynchronously notifies the
   worker to generate WebP, AVIF, and viewport-sized variants. The next
   request gets the optimized version.

The worker generates a set of cache variants per source
image, indexed by a 32-bit capability mask combining `Accept-Encoding`,
`Accept` MIME types, `Save-Data`, viewport class (mobile/tablet/desktop),
and pixel density. The mask is computed in C++ during request
classification; the actual selection is a hash lookup in the mmap'd cache.

## Verify

```bash
$ curl -sI -H "Accept: image/avif,image/webp,*/*" \
    http://localhost:5000/img/hero.jpg
HTTP/1.1 200 OK
Content-Type: image/avif
Vary: Accept, Save-Data, User-Agent
X-PageSpeed: HIT
Cache-Control: public, max-age=31536000, immutable
```

Same URL with a different `Accept`:

```bash
$ curl -sI -H "Accept: image/jpeg" \
    http://localhost:5000/img/hero.jpg
HTTP/1.1 200 OK
Content-Type: image/jpeg
Vary: Accept, Save-Data, User-Agent
X-PageSpeed: HIT
```

Same source file, two different content types, picked at serve time from
the request headers. The browser doesn't need a `<picture>` tag with
`<source type="image/webp">` fallbacks; the negotiation happens at the
URL level, transparent to markup. For the encoder tradeoffs behind that
choice — where AVIF beats WebP and where it doesn't — see
[AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/).

## Save-Data and viewport

Two additional axes the middleware honors automatically:

```bash
$ curl -sI -H "Accept: image/avif" -H "Save-Data: on" \
    http://localhost:5000/img/hero.jpg
HTTP/1.1 200 OK
Content-Type: image/avif
Content-Length: 18432    # ~40% smaller than the no-Save-Data variant
Vary: Accept, Save-Data, User-Agent
X-PageSpeed: HIT
```

When the browser sends `Save-Data: on` (Chrome's Lite Mode was discontinued
in 2022, but the signal persists through OS- and carrier-level data savers,
particularly on Android), the background service has pre-generated
lower-quality variants. The selection is automatic.

For viewport-aware sizing, the middleware reads the `Sec-CH-Viewport-Width`
client hint (where supported) or falls back to a User-Agent classification.
A 1920×1080 hero image served to a phone gets the 412px-wide variant; the
desktop gets the full size. Neither client downloads the other's bytes.

## LCP preload

In addition to image variants, the middleware adds preload hints for the
largest contentful paint candidate. The HTML processor identifies the
likely LCP image (first large `<img>` in the document, or one inside a
`hero`/`banner`-classed container) and emits a `Link` response header:

```
Link: </img/hero.jpg.pagespeed.ic.7a3f9e2c.webp>; rel=preload; as=image
```

This is a real response header on the HTML document; the browser starts
fetching the LCP image during the document parse, before it reaches the
`<img>` tag. Typical LCP improvement on hero-heavy landing pages is
200 to 600 ms on a cold 4G connection.

The nginx integration also emits `103 Early Hints` for this on cache
misses, sending preload headers before the origin response arrives.
Kestrel does not currently expose that capability to middleware, so the
ASP.NET path emits the hints as regular response headers only. See the
[architecture post](/blog/aspnet-core-middleware/) for the
nginx-vs-middleware comparison table.

## vs ImageSharp.Web

[ImageSharp.Web](https://docs.sixlabors.com/articles/imagesharp.web/index.html)
is the standard answer for ASP.NET Core image processing. It's a great
library; the comparison is about where the optimization happens.

|                         | ImageSharp.Web                    | WeAmp.PageSpeed                                      |
| ----------------------- | --------------------------------- | ---------------------------------------------------- |
| **Language**            | C#                                | C++ via P/Invoke                                     |
| **AVIF encoding**       | Not built-in (planned)            | Yes (libaom)                                         |
| **WebP encoding**       | Yes                               | Yes                                                  |
| **Content negotiation** | URL query string (`?format=webp`) | `Accept` header transparent                          |
| **Markup changes**      | `<img src="?format=webp">`        | None (the `<img>` tag is rewritten by the HTML pass) |
| **Viewport variants**   | Manual (`?width=412`)             | Automatic                                            |
| **LCP preload**         | No                                | Yes                                                  |
| **Critical CSS**        | No                                | Yes (out of scope for this post)                     |
| **License**             | Apache 2.0                        | Apache 2.0                                           |

ImageSharp.Web is the right choice if you want a focused, in-process
image library and you're happy to drive variants from query strings.

WeAmp.PageSpeed is the right choice if you want runtime content
negotiation against `Accept`, AVIF today, and the HTML-level optimization
passes (critical CSS, LCP preload, lazy-load injection) that the same
middleware provides as a side effect of being a PageSpeed pipeline rather
than an image library.

(Footnote: the two can coexist. ImageSharp.Web for build-time generation
and WeAmp.PageSpeed for runtime negotiation. The middleware doesn't care
whether the bytes it sees on disk came from a build step or a CDN.)

## Limitations

- **Versioning.** WeAmp.PageSpeed.AspNetCore is a stable NuGet package;
  install it without the `--prerelease` flag and read the release notes
  before moving to a new minor version.
- **HTML processing is async.** The first request for a new page gets
  the unprocessed HTML; the second request gets the rewritten version.
  For pre-generated static content, hit the URLs once during deploy to
  warm the cache.

## Configuration reference

Everything is hot-reloadable via `IOptionsMonitor<PageSpeedOptions>`:

```json
{
  "PageSpeed": {
    "Enabled": true,
    "Cache": {
      "VolumePath": "/var/cache/pagespeed/volume.dat",
      "VolumeSizeBytes": 1073741824
    },
    "Worker": {
      "AutoStart": true,
      "ApiPort": 9880
    }
  }
}
```

For the full options reference and how the cache volume sizing maps to
expected variants, see [ASP.NET Core
Configuration](/docs/aspnet-configuration/) or the
[NuGet package README](/go/nuget/?from=blog-aspnet-image-closer).

## Related

- [ModPageSpeed 2.0 now works with ASP.NET Core](/blog/aspnet-core-middleware/) — architecture deep dive
- [ASP.NET Core performance](/aspnet-core-performance/) — the broader server-layer picture for .NET and IIS
- [ASP.NET Core Getting Started](/docs/aspnet-getting-started/) — install + sidecar worker
- [ASP.NET Core Configuration](/docs/aspnet-configuration/) — full options reference
- [Critical CSS without a headless browser](/blog/critical-css-heuristics/) — what the same middleware does for CSS
- [Run ModPageSpeed 2.0 with Docker Compose](/blog/run-with-docker-compose/) — the nginx path for non-.NET stacks
- [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
- [Self-hosted image optimization](/self-hosted-image-optimization/) — keep transcoding on your own infrastructure
- [IISpeed alternative](/alternatives/iispeed/) — if you're on classic ASP.NET / IIS

---
title: 'Fix LCP on ASP.NET Core: Razor vs Blazor'
description: 'Fix LCP on ASP.NET Core: async view components, immutable cache headers, deferred JS and the WeAmp.PageSpeed middleware, for Razor Pages, MVC and Blazor.'
date: 2026-05-18
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'lcp', 'aspnet-core', 'blazor']
draft: false
product: '2.0'
howTo:
  name: How to fix LCP on ASP.NET Core
  description: Diagnose the LCP long pole, then optimize images and critical CSS in-process with the WeAmp.PageSpeed middleware while fixing async view components, asset caching, and deferred JS in the app.
  tools:
    - PageSpeed Insights
    - Chrome DevTools
    - Lighthouse
    - dotnet-counters
    - WeAmp.PageSpeed.AspNetCore (NuGet)
  steps:
    - name: Diagnose the LCP long pole
      text: Run PageSpeed Insights on the affected URL and read the Largest Contentful Paint element breakdown and TTFB. Record under Chrome DevTools Performance (Slow 4G + 4x CPU) and check the Network Timing tab's 'Waiting for server response' to determine whether the long pole is the server, the JS chain, or asset transport.
    - name: Recompress images and inline critical CSS via the middleware
      text: Add the WeAmp.PageSpeed.AspNetCore NuGet package, call services.AddPageSpeed(...) and app.UsePageSpeed(). Image recompression and WebP/AVIF transcoding plus critical-CSS inlining and LCP preload run in-process inside Kestrel automatically — there are no named filters to enable; you tune the HTML transforms with the options.Html.Enable* toggles.
    - name: Make view components async and cache hot data
      text: Mark layout-level view components async and cache nav and cart-summary data with IMemoryCache via _cache.GetOrCreateAsync, since no HTML rewriter can shorten a synchronous-I/O TTFB. Add an explicit preload link with fetchpriority=high for known LCP hero images.
    - name: Defer JS and set immutable cache headers
      text: 'Replace synchronous <script> tags in <head> with defer or move them to end-of-body, and configure StaticFileOptions.OnPrepareResponse to set Cache-Control: public, max-age=31536000, immutable on hashed assets. Extend ResponseCompression and add the Brotli provider explicitly.'
    - name: Measure the lift
      text: 'Re-run PageSpeed Insights and confirm smaller element render delay and load duration. In the DevTools Network panel, verify the hero is served as image/webp with the immutable Cache-Control header and X-PageSpeed: HIT on the second reload, then watch Search Console field data over 28 days.'
---

ASP.NET Core LCP problems split cleanly between two stacks: Razor Pages / MVC, where the fix is mechanical, and Blazor Server, where the fix is architectural. This post handles the first; the Blazor case gets a section near the end. Make synchronous view components async, fix the static-file cache headers, defer the bundled JS, then route image and CSS work through the [**WeAmp.PageSpeed ASP.NET Core middleware**](/blog/aspnet-core-middleware/) so the optimization runs in-process inside Kestrel.

## What LCP measures

LCP — Largest Contentful Paint — is the render time of the largest above-the-fold element on a page. On most ASP.NET Core sites that is the hero image rendered by `_Layout.cshtml`, a product photo on a Razor Pages catalog, or — on text-heavy pages — the `<h1>`. Under 2.5 s is "good"; over 4 s is "poor". The field number comes from Chrome's CrUX dataset (real users over the trailing 28 days), the lab number from Lighthouse and PageSpeed Insights — and the two diverge because Kestrel on your dev box is faster than the phones your visitors carry. See [web.dev/articles/lcp](https://web.dev/articles/lcp) for the canonical definition.

## The most common LCP failures on ASP.NET Core

Razor Pages and MVC sites have a manageable LCP problem. Blazor Server's is dominated by the initial SignalR handshake, so the platform-side fixes below help it less than they help Razor.

1. **Razor partial views fetch data synchronously.** A common pattern: `_Layout.cshtml` calls `@await Component.InvokeAsync("CartSummary")` and the view component does a database round-trip. The time-to-first-byte blows up; LCP cannot fire before the HTML headers arrive. On a typical e-commerce ASP.NET Core site, layout-level nav + cart-summary view components add 80–300 ms to TTFB.

2. **`bundle.min.js` is loaded synchronously in `<head>`.** Default Visual Studio templates put `<script src="~/lib/jquery/dist/jquery.min.js"></script>` and friends in the document head before the LCP image renders. Even with `defer`, the browser still has to fetch and parse them; if they share a connection with the hero image, the image fetch is delayed.

3. **Static files served without proper caching.** `app.UseStaticFiles()` ships defaults that don't set `Cache-Control: immutable`, so every repeat visitor still re-validates the LCP image. First-visit LCP also suffers because the default ResponseCompression middleware compresses text only — image bytes pass through uncompressed.

## Step 1: Diagnose

Before changing anything:

- Run PageSpeed Insights on the affected URL: `https://pagespeed.web.dev/analysis?url=<URL>`. Read the **Largest Contentful Paint element** breakdown. Pay attention to the TTFB number — for ASP.NET Core sites, large TTFB usually indicates a synchronous view-component or middleware doing I/O.
- Chrome DevTools → **Performance** panel → record under mobile throttling (Slow 4G + 4× CPU). Look at the timeline: a long flat bar before any document content suggests the server is the long pole.
- DevTools → Network panel → click the document request → **Timing** tab. Read **Waiting for server response**. If that number is > 200 ms on a warm cache, an upstream Razor partial is doing work.
- For repeatable measurement: `dotnet run --configuration Release` (don't profile Debug builds — they're 3–5× slower), then `npx lighthouse <URL> --only-categories=performance --form-factor=mobile`.
- Enable ASP.NET Core's request logging with `Microsoft.AspNetCore.Hosting` at `Information` level; `dotnet-counters monitor` on `Microsoft.AspNetCore.Hosting` gives request duration percentiles per endpoint.

Output you're hunting for: the LCP element, the TTFB number, and whether the long pole is the server (failure mode #1), the JS chain (failure mode #2), or asset transport (failure mode #3).

## Step 2: Recompress images and inline critical CSS in-process via the WeAmp.PageSpeed middleware

mod_pagespeed ships an ASP.NET Core middleware as a NuGet package (`WeAmp.PageSpeed.AspNetCore`). It runs the same optimization pipeline as the nginx deployment — image transcoding, CSS extraction, HTML rewriting, cache management — inside the Kestrel process via P/Invoke into a native shared library. Optimization is on by default; there are no named filters to enable, and you tune the HTML transforms with the `options.Html.Enable*` toggles. What moves LCP:

- **Image recompression and WebP/AVIF transcoding** — always-on worker behavior; it re-encodes hero images at a configured quality and transcodes to WebP and AVIF for browsers that advertise support. There is no toggle to switch it on.
- **Critical-CSS injection** (`options.Html.EnableCriticalCss`, default on) — extracts above-the-fold rules from the page's CSS and inlines them into the document `<head>`, so the browser can paint without waiting for the external stylesheet. See [how the critical-CSS heuristics decide what to inline](/blog/critical-css-heuristics/).
- **LCP preload** (`options.Html.EnableLcpPreload`, default on) — emits a `<link rel="preload">` hint for the detected LCP image so the browser fetches it early. This is one of the [server-injected resource hints](/blog/server-injected-resource-hints-speculation-rules/) the worker generates from the rendered HTML.

For a Razor Pages site where the LCP element is text rather than an image, mod_pagespeed's contribution is smaller — critical-CSS injection still helps by trimming the CSS chain, but the bigger win is fixing TTFB by making view components properly async.

Minimal `Program.cs` for an MVC/Razor Pages site:

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
app.MapRazorPages();
app.Run();
```

`AddPageSpeed()` registers the services; `UsePageSpeed()` inserts the middleware ahead of static files and routing. The full configuration reference, including the HTML toggles and worker setup, lives in [ASP.NET Core configuration](/docs/aspnet-configuration/) and [ASP.NET Core getting started](/docs/aspnet-getting-started/).

After enabling, re-run PSI. LCP attribution should show the WebP variant being served, with critical CSS inlined in the document `<head>`.

## What ASP.NET Core gives you out of the box

What you do inside the ASP.NET Core app to stack additional wins:

- Mark view components as `async` and cache hot data with `IMemoryCache`: register with `services.AddMemoryCache()`, then in the view component use `_cache.GetOrCreateAsync(...)` for the nav, cart-summary, and any other layout-level data. Even a 30-second cache eliminates most of the TTFB cost.
- Add an explicit `<link rel="preload" as="image" fetchpriority="high" href="@Url.Content("~/img/hero.webp")">` to `_Layout.cshtml` for known LCP images. This is the one place an explicit preload outperforms anything the rewriter can infer.
- Replace `<script src="..."></script>` in `<head>` with `<script defer src="...">`, or move scripts to end-of-body via `@RenderSection("Scripts", required: false)`. Default Visual Studio templates leave `jquery.min.js` in head — fix it.
- Configure `StaticFileOptions.OnPrepareResponse` to set `Cache-Control: public, max-age=31536000, immutable` on hashed asset paths under `wwwroot/`. This eliminates repeat-visit revalidation cost.
- `services.AddResponseCompression(options => options.MimeTypes = ResponseCompressionDefaults.MimeTypes.Concat(new[] { "image/svg+xml" }))` — extend the defaults; Brotli isn't enabled out of the box, add it explicitly via `options.Providers.Add<BrotliCompressionProvider>()`.

## Measuring the lift

1. Re-run PSI on the same URL. LCP element timing should show smaller element render delay (critical CSS inlined) and smaller resource load duration (WebP variant served).
2. DevTools → Network panel → reload, filter on `Img`, confirm the hero is served as `image/webp` with `Cache-Control: public, max-age=31536000, immutable`.
3. DevTools → Network panel → check the document response headers. The middleware adds `X-PageSpeed: HIT` on a cache hit and `X-PageSpeed: MISS` on the first request; you should see `HIT` on the second reload.
4. After 28 days, check Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" to "Good". Watch field data, not just lab.

## Configuration cheat sheet

```csharp
// Program.cs — minimal config to address LCP on ASP.NET Core
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = "/data/cache.vol";
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;
    options.Worker.SocketPath = "/data/pagespeed.sock";

    // Image recompression and WebP/AVIF transcoding are always-on worker
    // behavior — no toggle. Critical CSS and LCP preload default to on;
    // shown here for clarity. Tune the rest via options.Html.Enable*.
    options.Html.EnableCriticalCss = true;
    options.Html.EnableLcpPreload = true;
});

builder.Services.AddResponseCompression(options =>
{
    options.EnableForHttps = true;
    options.Providers.Add<BrotliCompressionProvider>();
});

var app = builder.Build();
app.UsePageSpeed();
app.UseResponseCompression();
app.UseStaticFiles(new StaticFileOptions
{
    OnPrepareResponse = ctx =>
        ctx.Context.Response.Headers.CacheControl =
            "public, max-age=31536000, immutable",
});
app.MapRazorPages();
app.Run();
```

The full configuration reference lives in [ASP.NET Core configuration](/docs/aspnet-configuration/). The middleware also runs in [standalone mode](/blog/aspnet-core-middleware/) without the worker, providing HTML-only optimizations (critical CSS, image dimensions, preload hints) if you don't want to deploy the worker process.

## When this doesn't work

Cases where mod_pagespeed alone isn't enough on ASP.NET Core:

- **Blazor Server.** The initial paint is a SignalR handshake. mod_pagespeed cannot rewrite content that's streamed over a WebSocket. The architectural fix is Blazor WebAssembly or `@rendermode InteractiveAuto` (.NET 8+), which falls back to WASM after the first visit.
- **TTFB is the long pole and view components are still synchronous.** No HTML rewriter can shorten a 1 s TTFB; fix the synchronous I/O first. Run `dotnet-trace collect` on the process, look at the BlockingTime counter, and convert blocking work to `async`/`await`.
- **The LCP is a YouTube embed or third-party iframe.** mod_pagespeed cannot rewrite third-party iframe content. Use a static-poster facade pattern: render a `<picture>` with the video thumbnail and a play button; replace it with the iframe on click. The LCP becomes your own image.
- **The site is a Single-Page App with ASP.NET Core only as the API layer.** Build-time image optimization (Webpack, Vite, esbuild) is the right layer for SPA LCP work; the middleware doesn't see the SPA's render pipeline.

## Related

- [How to fix INP on ASP.NET Core](/blog/fix-inp-aspnet-core-2026/)
- [How to fix CLS on ASP.NET Core](/blog/fix-cls-aspnet-core-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [ModPageSpeed 2.0 now works with ASP.NET Core](/blog/aspnet-core-middleware/)
- [Image optimization in ASP.NET Core](/blog/aspnet-core-image-optimization-c-sharp/)
- [IIS and Core Web Vitals in 2026](https://iispeed.com/iis-core-web-vitals-2026/)
- [ASP.NET Core getting started](/docs/aspnet-getting-started/)
- [The full LCP guide](/core-web-vitals/lcp/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as ASP.NET Core middleware (NuGet) or as a module for Apache, nginx and IIS. The IIS module ships as a signed MSI on the mod_pagespeed GitHub release. On ASP.NET Core, install the middleware and run your app — it optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).

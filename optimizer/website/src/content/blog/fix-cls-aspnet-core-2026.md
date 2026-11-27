---
title: 'Fix CLS on ASP.NET Core: pre-size async holes'
description: 'How to fix CLS on ASP.NET Core: pre-size async view components, persist image dimensions, reserve banner space, rewrite img tags via middleware.'
date: 2026-05-19
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'cls', 'aspnet-core', 'layout-shift']
draft: false
howTo:
  name: How to fix CLS on ASP.NET Core
  description: Fix Cumulative Layout Shift on ASP.NET Core by rewriting img dimensions in-process and pre-sizing the async holes the framework defers.
  tools:
    - PageSpeed Insights
    - Chrome DevTools
    - WeAmp.PageSpeed.AspNetCore
    - SixLabors.ImageSharp
    - web-vitals
  steps:
    - name: Confirm it is a CLS problem
      text: Run PageSpeed Insights on the affected URL and read the "Avoid large layout shifts" diagnostic. In Chrome DevTools, enable Web Vitals and Layout Shift Regions, throttle CPU 4x, and reload; optionally log onCLS from the web-vitals package in a dev-only Razor block to attribute the worst element to one of the three failure modes.
    - name: Insert img dimensions via the middleware
      text: 'Add the WeAmp.PageSpeed.AspNetCore middleware; its always-on image-dimensions transform (default on, gated by options.Html.EnableImageDimensions) inspects the outbound HTML, reads each underlying file, and writes width and height onto every <img> missing them so the browser reserves the box. Test it on responsive templates first, and pair it with the CSS rule img { max-width: 100%; height: auto; }.'
    - name: Persist image dimensions on the view model
      text: For uploaded images, read pixel dimensions once at upload time with SixLabors.ImageSharp and persist Width and Height to the view model, then render explicit width/height attributes. This is the durable fix; the middleware rewrite becomes a no-op when the attributes are already present.
    - name: Pre-size async view components with a skeleton
      text: Wrap the empty mount point of an @await Component.InvokeAsync(...) call in a div with a min-height matching the expected populated height. The slot stays the same size when the component completes, so no shift fires. The middleware cannot fix this because it only operates on bytes the response stream emits.
    - name: Render cookie banners and Blazor mounts server-side
      text: Switch cookie-banner injection to inline server-side markup with CSS visibility driven by a server-set cookie, avoiding JS-driven appearance. For Blazor Server, wrap components that may expand on hydration in a div with a matching min-height so the asynchronous mount does not record a shift.
    - name: Measure the lift
      text: Re-run PSI until "Avoid large layout shifts" lists zero shifted elements, confirm an empty Layout Shift Regions overlay in a throttled DevTools trace, then watch the CrUX field CLS in the PSI Origin panel and the Search Console Core Web Vitals report over the following weeks.
---

CLS on ASP.NET Core splits into two markup-hygiene problems: `<img>` tags missing dimensions (uploaded files where the view model dropped `Width`/`Height`), and async view components that mount empty boxes via `@await Component.InvokeAsync(...)`. The first is solved at the response-stream layer by the [**WeAmp.PageSpeed ASP.NET Core middleware**](/blog/aspnet-core-middleware/) — its always-on HTML transform reads each underlying file once and injects `width`/`height` onto every `<img>` that lacks them, for every Razor view that emits `<img>`. The second is solved by sizing the async holes the framework explicitly chose to defer.

## What CLS measures

Cumulative Layout Shift is the sum of every unexpected content movement between load start and the moment the page becomes interactive. Each shift counts as the share of the viewport that moved multiplied by how far it travelled, and the worst session window's total is the score Google reports. Per [web.dev/cls](https://web.dev/articles/cls), under 0.1 is "good" and 0.25 is where "poor" begins.

Shifts inside 500 ms of a user interaction are excluded. The metric you optimize is the field 75th-percentile CLS from CrUX, shown in PSI's "Origin" or "Page" CrUX panel. Lighthouse lab CLS is a single synthetic run and often disagrees with the field — especially for ASP.NET Core sites where async view components hydrate on a timing the lab doesn't reproduce.

## The most common CLS failures on ASP.NET Core

Three patterns cover the bulk of ASP.NET Core CLS regressions.

**Async view components mount late and fill empty boxes.** `@await Component.InvokeAsync("RelatedProducts")` returns markup that does not exist in the initial response. If the view component fetches data from a downstream API (catalog, recommendations, inventory), the wait happens on the request thread but the placeholder mounted in the parent view is empty. Renderers using `IViewComponentResult` with caching headers behave more predictably, but the common pattern — async component, no skeleton, no reserved height — produces a textbook CLS hit.

**Razor `<img>` tags from `IFormFile` uploads lack dimensions.** Admin pattern: a user uploads a JPEG via `IFormFile`, the controller saves it to `wwwroot/uploads/` or blob storage, the view model carries `string ImageUrl`. The Razor view renders `<img src="@Model.ImageUrl" alt="...">` with no `width` or `height`. The dimensions are known at upload time (the file's pixel dimensions can be read in 1 ms with `SixLabors.ImageSharp`) but rarely persisted to the view model. Every catalog page, every news article, every uploaded-image surface suffers the same shift.

**Cookie consent banners injected by middleware.** `Microsoft.AspNetCore.CookiePolicy` and similar third-party packages inject a top-of-page banner on first visit through a `IHtmlGenerator` hook. The banner is in the response stream but the JavaScript that controls its visibility runs after the initial paint; the banner appears, the main content pushes down, CLS records the shift. Banners injected via JavaScript only (no server-side markup) are even worse.

## Confirm it's actually a CLS problem

Run PSI on the affected URL: `https://pagespeed.web.dev/analysis?url=https://example.com/`. The "Avoid large layout shifts" diagnostic lists the elements; on ASP.NET Core the worst offender is usually an async-loaded section or a `<img>` tag rendered from a view-model URL.

In Chrome DevTools: Performance panel → Settings cog → enable "Web Vitals" and "Layout Shift Regions". Throttle CPU to 4×. Reload. The async view component appearing late shows as a red rectangle at the section's location; the un-sized image shows as a red rectangle wherever the `<img>` sits in the document flow.

The `web-vitals` npm package logged from a dev-only Razor block gives you precise attribution:

```cshtml
@if (Env.IsDevelopment())
{
    <script type="module">
      import { onCLS } from 'https://unpkg.com/web-vitals?module';
      onCLS(console.log, { reportAllChanges: true });
    </script>
}
```

The console prints the worst element per session. Match it to one of the three failure modes.

## Insert img dimensions in-process via the WeAmp.PageSpeed middleware

WeAmp.PageSpeed.AspNetCore — the NuGet middleware that wraps the same optimization pipeline as the nginx integration — handles the `<img>`-dimensions problem with no configuration: it is on by default. The middleware sits in the response pipeline, inspects the outbound HTML stream, reads the underlying file (cached after first hit; works with `wwwroot/` paths and with absolute URLs your origin serves), and inserts `width` and `height` on every `<img>` missing them. Combined with the bootstrap CSS rule `img { max-width: 100%; height: auto; }` (or Tailwind's equivalent), the browser reserves the correct aspect-ratio box before pixels arrive. The "image jump" shift disappears across every Razor view, every Blazor component that emits `<img>`, every Markdown-rendered article body.

The image-dimensions transform defaults to on; if you need to gate it, the toggle is `options.Html.EnableImageDimensions`. Writing fixed `width`/`height` onto markup that previously relied on CSS for sizing can interfere with some responsive layouts — the `img { max-width: 100%; height: auto; }` rule covers the standard case, but custom Razor components and CMS-imported article bodies vary, so test on your responsive templates before relying on it everywhere.

Lazy loading (`options.Html.EnableLazyLoad`, default on) reserves dimensioned placeholders for offscreen images and defers the fetch. Because the placeholder is sized, no shift fires when the real image swaps in.

Critical-CSS injection (`options.Html.EnableCriticalCss`, default on) inlines above-fold CSS into the document head and defers the rest. On ASP.NET Core sites that ship Bootstrap, Tailwind, or a large compiled stylesheet from `_Layout.cshtml`, this measurably reduces the FOUC window during which fonts and layout can shift.

Why a rewriter beats auditing every Razor file: it applies the fix uniformly, including content from CMS imports, Markdown converters, and third-party Razor packages whose templates you don't control. Per-file auditing works for green-field projects; the middleware works for the inherited ones.

What this does not fix: the async-view-component shifts, the cookie-banner inject, and the Blazor Server hydration mount. The rewriter can only operate on bytes the response stream actually emits — it cannot pre-size content that the framework explicitly chose to defer.

## Fix the shifts the middleware can't reach

- For uploaded images, persist `Width` and `Height` to the view model on upload. Read them once with `SixLabors.ImageSharp`:

```csharp
using var image = await Image.LoadAsync(file.OpenReadStream());
upload.Width = image.Width;
upload.Height = image.Height;
```

Then render with explicit attributes: `<img src="@Model.ImageUrl" width="@Model.Width" height="@Model.Height" alt="...">`. The middleware's rewrite becomes a no-op (it only inserts when attributes are missing); the dimensions are correct from the first byte.

- For async view components, replace the empty mount point with a pre-sized skeleton. `_Layout.cshtml` or the parent view:

```cshtml
<div class="related-products-skeleton" style="min-height: 320px;">
  @await Component.InvokeAsync("RelatedProducts")
</div>
```

The skeleton's `min-height` matches the expected populated height; when the component completes, the slot stays the same size and no shift fires.

- Switch cookie-banner injection to server-side rendering. `Microsoft.AspNetCore.CookiePolicy` can emit the banner markup inline in `_Layout.cshtml`; CSS controls visibility based on a cookie set by middleware. No JS-driven appearance, no shift.

- For Blazor Server: wrap server-rendered components that may expand on hydration in a `<div>` with `min-height` matching the expected post-hydration size. Hydration happens asynchronously over SignalR; CLS records the expansion if you let the component mount into a zero-height box.

- For static images shipped from `wwwroot/`, build-time tooling can also inject dimensions. The middleware approach is more general because it covers user-uploaded content; the build-time approach is faster (no per-request inspection). Both stack.

## Measuring the lift

1. Re-run PSI. "Avoid large layout shifts" should drop to zero shifted elements or list only the cookie banner if you haven't moved it server-side.
2. DevTools → Performance → record a reload trace with 4× CPU throttle. The Layout Shift Regions overlay should be empty across the page.
3. After 28 days, Search Console → Core Web Vitals report. The URL group should move from "Needs improvement" to "Good".
4. Watch the CrUX field CLS in PSI's "Origin" panel. The lab Lighthouse CLS sometimes reports 0 because the lab doesn't trigger the async view-component path; the field is the truth.

## The drop-in middleware config

For ASP.NET Core via the WeAmp.PageSpeed.AspNetCore NuGet package, the minimal startup snippet:

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = "/data/cache.vol";
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;
    options.Worker.SocketPath = "/data/pagespeed.sock";

    // These HTML transforms default to on; shown here for clarity.
    // There are no named filters — you tune the transforms via options.Html.
    options.Html.EnableImageDimensions = true;
    options.Html.EnableLazyLoad = true;
    options.Html.EnableCriticalCss = true;
});

var app = builder.Build();
app.UsePageSpeed();
```

If you front Kestrel with nginx or IIS instead of running the middleware in-process, use the equivalent server-level configuration from the [ASP.NET Core middleware post](/blog/aspnet-core-middleware/) or the IIS integration referenced in `/iis-core-web-vitals-2026/` on iispeed.com.

## When this doesn't work

- If CLS attribution still names the async view component after the skeleton change, the component's populated height is highly variable (e.g., a "recently viewed" list with 0–10 items). Pick a fixed `min-height` that matches the empty state, not the populated one — an empty box is shift-free; a sometimes-empty, sometimes-full box always shifts.
- If `<img>` shifts persist after the dimensions transform should have run, the middleware is not running on the response (check `app.UsePageSpeed()` ordering — it must run before any compression or response-caching middleware), the transform is gated off (`options.Html.EnableImageDimensions`), or the images are served from a CDN the middleware cannot fetch metadata from. Persist dimensions to the view model as the durable fix.
- Blazor WebAssembly: the rewriter sees only the initial app-shell HTML, not the components rendered client-side. CLS in Blazor WASM is a `min-height` problem on every component container — solve in CSS.
- If CLS attribution names a cookie banner that you have already moved server-side, the JS that _hides_ the banner on accept fires after the initial paint, and that hide event records as a shift. Hide the banner with a server-set cookie + CSS `display: none` from the start of the page lifetime; no JS-driven visibility change.

## Related

- [How to fix LCP on ASP.NET Core](/blog/fix-lcp-aspnet-core-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix INP on ASP.NET Core](/blog/fix-inp-aspnet-core-2026/)
- [How to fix CLS on nginx](/blog/fix-cls-nginx-2026/)
- [ModPageSpeed 2.0 as ASP.NET Core middleware](/blog/aspnet-core-middleware/)
- [IIS Core Web Vitals 2026 (iispeed.com)](https://iispeed.com/iis-core-web-vitals-2026/)
- [The full CLS guide](/core-web-vitals/cls/)
- [Test your page in the analyzer](/analyze/)

mod_pagespeed runs as ASP.NET Core middleware (NuGet) or as a module for Apache, nginx and IIS. The IIS module ships as a signed MSI on the mod_pagespeed GitHub release. On ASP.NET Core, install the NuGet package and run your app — it optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).

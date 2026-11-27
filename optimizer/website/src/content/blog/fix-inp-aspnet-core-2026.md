---
title: 'Fix INP on ASP.NET Core: Razor vs Blazor'
description: 'How to fix INP on ASP.NET Core: Razor + jQuery responds well to bundling and the WeAmp.PageSpeed middleware; Blazor Server INP is architectural (WASM).'
date: 2026-05-20
author: 'Otto van der Schaaf'
tags: ['core-web-vitals', 'inp', 'aspnet-core', 'blazor']
draft: false
product: '2.0'
lastUpdated: 2026-10-22
howTo:
  name: How to fix INP on ASP.NET Core
  description: Diagnose the worst interaction on ASP.NET Core, then reduce JavaScript parse and transfer cost on Razor Pages / MVC with the WeAmp.PageSpeed middleware and framework-level tuning, while routing architectural Blazor Server INP to a rendering-model change.
  tools:
    - WeAmp.PageSpeed ASP.NET Core middleware
    - Chrome DevTools
    - web-vitals
    - PageSpeed Insights
    - Google Search Console
    - WebOptimizer
  steps:
    - name: Identify the worst interaction
      text: Record the user flow in Chrome DevTools Performance panel and read the Interactions track to name the slowest interaction. Add the web-vitals library with attribution in _Layout.cshtml to log onINP and identify the responsible handler, and read the field-data INP in PageSpeed Insights. For Blazor Server, inspect the SignalR WebSocket frame round-trip time in the Network panel.
    - name: Reduce parse and transfer cost on Razor Pages / MVC
      text: For Razor Pages / MVC, enable the WeAmp.PageSpeed ASP.NET Core middleware in Program.cs. JavaScript minification is always-on worker behavior; script deferral (options.Html.EnableScriptDeferral, default on) pushes the scripts its browser analysis proves safe past first paint so jQuery parse no longer collides with the first click. Stage deferral first since it changes execution order. Blazor Server is out of scope here because no response-stream rewriter can shorten a SignalR network round-trip.
    - name: Apply framework-level tuning
      text: On Razor Pages / MVC, replace jQuery validation with native HTML5 validation, bundle and minify build-time assets, and move scripts to end-of-body instead of the head. On Blazor Server, switch to Blazor WebAssembly or @rendermode InteractiveAuto, avoid StateHasChanged() near the tree root, use ShouldRender() overrides, and pre-render non-interactive routes.
    - name: Confirm the fix took
      text: Compare web-vitals onINP attribution logs before and after deploy and re-record the DevTools Performance flow to confirm the worst interaction is under 200 ms on Razor/MVC. Re-run PageSpeed Insights and check the field INP, then review the Search Console Core Web Vitals report after 28 days. For Blazor Server, re-check the SignalR frame round-trip, which is the floor the app can approach but not go below.
---

ASP.NET Core has two different INP profiles depending on the frontend. Razor Pages / MVC with vanilla JS or jQuery has a fixable INP problem — the [**WeAmp.PageSpeed ASP.NET Core middleware**](/blog/aspnet-core-middleware/) runs the same in-process optimization pipeline, which minifies JavaScript and defers the scripts its browser analysis proves safe — so startup script work stops competing for the main thread. Blazor Server has an INP problem that is **architectural and unfixable without changing the rendering model**: every interaction is a SignalR round-trip, which means INP is bounded below by network latency plus server render time. Both cases covered below.

## What INP measures

Interaction to Next Paint (INP) is the worst-case latency between a user input — click, tap, keypress — and the next frame the browser paints in response. Chrome watches every interaction across the page's lifetime and reports the 98th-percentile slowest one as that page's INP. Good is **≤ 200 ms**, "needs improvement" runs to 500 ms, and past 500 ms is failing. Real-user numbers come from CrUX; the lab approximation in PSI is unreliable here because Lighthouse drives no human input — so on ASP.NET Core, where Blazor's interaction model never even runs in the lab, the field number is the only one that matters. See [web.dev/inp](https://web.dev/articles/inp).

## The most common INP failures on ASP.NET Core

The failures split cleanly along the rendering-model line. Pick the section that matches your stack.

**Razor Pages / MVC with vanilla JS or jQuery:**

1. **`bundle.min.js` loaded synchronously in `<head>`.** Visual Studio's default templates still put `<script src="~/lib/jquery/dist/jquery.min.js"></script>` (and `bootstrap.bundle.min.js`, and `jquery-validation`) in `<head>` ahead of the LCP element. Even with `defer`, the parse + init still has to complete before the first interaction can be handled — typically 100–250 ms of cost on a mid-range Android.
2. **Anti-forgery token validation on POST inflates handler latency.** Razor Pages auto-validate the anti-forgery token on every POST handler. If the handler also does a DB call, the synchronous validate-then-handle path means form-submit INP includes network + token-validate + handler. 300 ms+ is common.
3. **jQuery validation runs synchronously on every keystroke.** `jquery.validate.unobtrusive.js` attaches `keyup` and `blur` validators that re-run on every input event. On a form with 8+ fields, the per-keystroke handler can hit 30–60 ms, recorded as INP on the worst keystroke.

**Blazor Server:**

1. **Every interaction is a SignalR round-trip.** Blazor Server renders on the server and pushes DOM diffs over SignalR. INP on a button click = network latency + server render time + diff apply. For a transatlantic user (~150 ms RTT), the floor before any work is done is already at the "Needs improvement" boundary.
2. **Disconnected-then-reconnect spikes INP.** SignalR reconnect on a flaky network causes the next interaction's INP to balloon to seconds. Reconnect handling is by design but is INP-visible.
3. **Component re-render on parent state change cascades to children.** A `StateHasChanged()` call near the top of the component tree triggers re-render of everything below; the diff sent over SignalR can be megabytes. INP on the click that triggered it is the wait for the full diff to apply.

## Identify the worst interaction

Identifying _which interaction_ records the bad INP is more important on ASP.NET Core than elsewhere because the answer dictates whether MPS is even the right tool.

- Open the page in Chrome, DevTools → **Performance** panel, record, perform a realistic user flow (nav click, form fill, submit). Stop. The **Interactions** track names the worst interaction; clicking it shows the flame chart.
- Install [`web-vitals`](https://github.com/GoogleChrome/web-vitals) with `attribution` in `_Layout.cshtml`. Log `onINP(console.log, {reportAllChanges: true})` from a small `<script type="module">` block. The `attribution.eventEntry` field names the responsible handler.
- Run PSI: `https://pagespeed.web.dev/analysis?url=<URL>`. Lab INP numbers are unreliable; the field-data section under "Discover what your real users are experiencing" is the one to read.
- For Blazor Server specifically: open DevTools → **Network** → filter for "WebSocket". Each interaction shows as a frame on the SignalR connection. The time from frame-send to frame-receive is the network floor on your INP.

Output: a specific interaction, the script (or `_blazor` SignalR frame), and whether the cost is parse, handler logic, or network round-trip.

## Reduce parse and transfer cost on Razor Pages / MVC

Blazor Server is out of scope for this step; skip to the framework-level tuning below.

**For Razor Pages / MVC sites**, the [WeAmp.PageSpeed ASP.NET Core middleware](/blog/aspnet-core-middleware/) (NuGet) runs the same in-process optimization pipeline as the nginx interceptor. Two behaviors lower INP, both on by default:

- **[JavaScript minification](/blog/safe-javascript-minification-semicolon-insertion/)** — always-on worker behavior; it strips inline and external JS down. The `.min.js` files are already minified, but inline `<script>` blocks in Razor partials usually are not. There is no toggle to switch it on.
- **Script deferral** (`options.Html.EnableScriptDeferral`, default on) — defers the scripts the worker's browser analysis proves safe, so jQuery + jQuery Validation parse no longer collides with the first click. Because it changes execution order — code in `<script>` blocks that depends on `$(document).ready` running before other scripts may break — stage and profile it before relying on it in production.

Wire it up in `Program.cs`:

```csharp
using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = "/data/cache.vol";
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;

    // JS minification is always-on worker behavior — no toggle.
    // Script deferral defaults to on; shown here for clarity.
    options.Html.EnableScriptDeferral = true;
});

var app = builder.Build();
app.UsePageSpeed();
```

**For Blazor Server**, mod_pagespeed cannot help. SignalR round-trips are the dominant INP cost; no rewriter operating on the response stream can shorten a network round-trip. Skip this step entirely and go straight to the framework-level tuning below.

## Where the framework still helps

**For Razor Pages / MVC:**

- Replace jQuery validation with native HTML5 form validation (`required`, `type="email"`, `pattern=`) + a 1 KB error-display helper. Drops a 30+ KB JS dependency and removes per-keystroke validator handlers — the same [remove-unused-JavaScript](/blog/remove-unused-javascript-chrome-coverage/) logic that lowers parse cost at the source.
- Bundle and minify with [WebOptimizer](https://github.com/ligershark/WebOptimizer) (`Microsoft.AspNetCore.WebOptimizer.Core` package). It outputs hashed, gzipped assets at build time.
- Use `<script type="module">` for ESM-style code paths. Modern browsers parse modules in a separate scheduler with lower INP impact than classic-script parse.
- Cache anti-forgery tokens client-side per session — reuse across forms instead of re-validating each.
- Move scripts to end-of-body via `@RenderSection("Scripts", required: false)` in `_Layout.cshtml`, not `<head>`.

**For Blazor Server:**

- **Switch to Blazor WebAssembly**, or use `@rendermode InteractiveAuto` (.NET 8+) to fall back to WASM after the second visit. WASM runs in-browser, so INP is bounded by client-side compute instead of network round-trip.
- If you must stay on Blazor Server: keep the user geographically close to the server. A US-region server with EU users has a 100+ ms RTT floor that no application change can remove.
- Avoid `StateHasChanged()` near the component-tree root. Push state down so the re-render fan-out is small.
- Use `ShouldRender()` overrides to skip diff computation on components whose state has not changed.
- Pre-render to static HTML for non-interactive routes (`@rendermode StaticSSR`) — INP is irrelevant there because there is no interaction.

## How to confirm it took

1. Compare `web-vitals` `onINP` attribution logs (from the Diagnose step) before and after deploy. The worst handler should be different — and shorter — than the baseline; for Razor/MVC sites the slowest entry should now sit in the "Good" band.
2. DevTools → Performance, re-record the user flow, confirm the **Interactions** track shows under-200 ms for the worst interaction.
3. For Blazor Server: re-check the SignalR frame round-trip time in DevTools → Network → WebSocket. That number is your floor; the application can be optimized down to it but not below.
4. Re-run PSI on the same URL. Field INP under "Interaction to Next Paint" should drop into the "Good" band for Razor/MVC sites after the rewriter + platform tuning. Blazor Server sites usually cannot exit "Needs improvement" without a re-architecture.
5. Wait 28 days, check Search Console → Core Web Vitals report. ASP.NET Core sites typically have a single URL group; track it as a whole.

## The middleware config

```csharp
// Program.cs — minimal config to address INP on ASP.NET Core (Razor/MVC)
using WeAmp.PageSpeed.AspNetCore;

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = "/data/cache.vol";
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;

    // JS minification is always-on worker behavior — no toggle.
    // Script deferral defaults to on; shown here for clarity.
    options.Html.EnableScriptDeferral = true;
});

// In the pipeline, before MVC:
app.UsePageSpeed();
```

For sites still fronted by nginx (e.g., Kestrel behind nginx reverse proxy), the [nginx config from the WordPress post](/blog/fix-inp-wordpress-2026/) works unchanged.

**For Blazor Server: there is no MPS config that fixes INP. See "Where this approach falls short" below.**

## Where this approach falls short

The Blazor Server section, expanded because it dominates the failure modes here.

- **Blazor Server INP is architectural, not an optimization problem.** SignalR round-trips are by design; the framework was built for interactive line-of-business apps on a LAN, not public sites served to users on mobile. If you are seeing INP > 500 ms on a public Blazor Server site, mod_pagespeed and every other server-layer tool will fail to move the number meaningfully. The fix is Blazor WebAssembly, `@rendermode InteractiveAuto`, or a different frontend. We will not pretend otherwise.
- **Razor Pages with third-party JS controls.** Telerik, Syncfusion, and DevExpress UI controls ship their own JS frameworks. INP on a Telerik grid sort or DevExpress combo open is bounded by their library's render path — MPS minifies their bytes but cannot change their internal architecture.
- **TTFB-driven INP on uncached XHR endpoints.** If your interactive widgets fire `fetch('/api/something')` and your API endpoint takes 300 ms to respond, INP on that interaction is at least 300 ms. The fix is server-side — caching, async/await discipline in handlers, or DB indexing. Not page optimization.
- **SignalR reconnect spikes.** Even on Blazor WebAssembly, if you use SignalR for real-time updates and the connection drops, the next interaction may record a multi-second INP. Implement client-side queuing.
- **`StateHasChanged()` on hot paths.** A Blazor component that re-renders on every prop change cascades through its children. Profile with Blazor's built-in diagnostic counters before assuming MPS is the answer.

ASP.NET Core's INP story is almost entirely about what frontend you ship. mod_pagespeed handles the Razor / jQuery case cleanly; the Blazor Server case is one we route to the framework, not to ourselves.

## Related

- [How to fix LCP on ASP.NET Core](/blog/fix-lcp-aspnet-core-2026/)
- [How to fix Largest Contentful Paint (LCP)](/core-web-vitals/lcp/): the four LCP phases and the server-side fixes
- [How to fix CLS on ASP.NET Core](/blog/fix-cls-aspnet-core-2026/)
- [How to fix Cumulative Layout Shift (CLS)](/core-web-vitals/cls/): causes, thresholds and the server-side fixes
- [How to fix INP on nginx (generic)](/blog/fix-inp-nginx-2026/)
- [ModPageSpeed 2.0 as ASP.NET Core middleware](/blog/aspnet-core-middleware/)
- [mod_pagespeed filters reference](/docs/filters/)
- [The full INP guide](/core-web-vitals/inp/)
- [Test your page in the analyzer](/analyze/)

The IIS-flavored angle on the same metrics, including IIS-specific deployment notes, is at [iispeed.com — IIS Core Web Vitals 2026](https://iispeed.com/iis-core-web-vitals-2026/).

mod_pagespeed runs as ASP.NET Core middleware (NuGet) or as a module for Apache, nginx and IIS. The IIS module ships as a signed MSI on the mod_pagespeed GitHub release. On ASP.NET Core, install the middleware and run your app — it optimizes out of the box. See [pricing](/pricing/) and [license terms](/license/).

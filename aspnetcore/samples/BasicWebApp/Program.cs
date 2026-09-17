// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using WeAmp.PageSpeed.AspNetCore.DependencyInjection;

var builder = WebApplication.CreateBuilder(args);

// PageSpeed (mod_pagespeed 1.15) — reads PageSpeed:* from appsettings.json. The
// DEFAULT topology is now Inverse: THIS Kestrel app is the public front door (it
// owns the public socket/TLS/auth/routing) and the bundled (nginx +
// ngx_pagespeed.so) matched pair runs LOOPBACK-ONLY behind it as an optimize-proxy
// (SidecarMode.Inverse). It is the default because the operator then has no
// separate front proxy to run, terminate TLS on, or keep in sync with the app: the
// middleware streams optimizable responses to nginx, which proxy_pass-es back to a
// private raw-origin Kestrel endpoint where the middleware bypasses itself.
//
// In Inverse the operator MAY own the public bind (the common edge-TLS case):
// configure your public endpoint via --urls / UseUrls / Kestrel:Endpoints, or set
// PageSpeed:Sidecar:OwnPublicPort=true to let the package auto-bind ListenPort.
// (For the classic front-proxy topology where nginx owns the public port, use
// AddPageSpeedProcess() instead.)
builder.Services.AddPageSpeed(builder.Configuration);

var app = builder.Build();

// Inverse: UsePageSpeed() is the real request-path middleware. Place it AFTER
// routing/auth (Kestrel owns auth) and BEFORE endpoint mapping; place it BEFORE any
// UseForwardedHeaders so the loopback transport-peer assertion reads the
// un-rewritten Connection.RemoteIpAddress.
//
// NOTE on compression: if you add ResponseCompression middleware, order it AFTER
// UsePageSpeed (or scope it off the raw-origin path) so nginx receives identity-
// encoded HTML. The generated nginx config also forces Accept-Encoding identity on
// the nginx->raw-origin hop as a backstop.
app.UsePageSpeed();
app.MapHealthChecks("/health");
app.MapPageSpeedHealthCheck("/health/pagespeed");
app.MapPageSpeedInfo("/pagespeed/info");

// Origin HTML referencing an EXTERNAL stylesheet so PageSpeed fetches, minifies
// and inlines it (CoreFilters: rewrite_css + inline_css). Prove optimization via
// the minified <style> + the X-Page-Speed header + css_filter_* stats — NEVER a
// ".pagespeed." substring (it false-positives whenever the page's own text
// happens to mention it).
app.MapGet("/", () => Results.Content("""
    <!DOCTYPE html>
    <html>
    <head>
        <title>PageSpeed nginx sidecar sample</title>
        <link rel="stylesheet" href="/style.css">
    </head>
    <body>
        <h1>mod_pagespeed 1.1 as an ASP.NET Core nginx sidecar</h1>
        <p>If optimization is active, the stylesheet is fetched, minified and
        inlined into this page.</p>
        <ul>
            <li><a href="/health">/health</a> — overall health check</li>
            <li><a href="/health/pagespeed">/health/pagespeed</a> — PageSpeed-specific health</li>
            <li><a href="/pagespeed/info">/pagespeed/info</a> — sidecar status (never the admin token)</li>
        </ul>
    </body>
    </html>
    """, "text/html"));

// A cacheable, whitespace-heavy stylesheet PageSpeed can minify + inline.
app.MapGet("/style.css", (HttpContext ctx) =>
{
    ctx.Response.Headers.CacheControl = "public, max-age=600";
    return Results.Content(
        "body   {   color :   #333 ;   font-family :   Arial , sans-serif ;   margin :   40px ;   }\n" +
        "h1     {   color :   #00aa77 ;   }\n",
        "text/css");
});

// An API path excluded from optimization (PageSpeed:ExcludePaths).
app.MapGet("/api/data", () => new { message = "API endpoints bypass PageSpeed", timestamp = DateTime.UtcNow });

app.Run();

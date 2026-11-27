// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);

var cachePath = Environment.GetEnvironmentVariable("PAGESPEED_CACHE_PATH")
    ?? Path.Combine(Path.GetTempPath(), "pagespeed-demo", "volume.dat");
var socketPath = Environment.GetEnvironmentVariable("PAGESPEED_SOCKET_PATH");
var apiPort = int.TryParse(Environment.GetEnvironmentVariable("PAGESPEED_API_PORT"), out var p) ? p : 0;

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = cachePath;
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;
    options.Worker.ApiPort = apiPort;
    // DemoSite is driven entirely by environment:
    //   PAGESPEED_SOCKET_PATH set (docker-compose split-process): pin the
    //     shared socket the separate worker container binds.
    //   PAGESPEED_SOCKET_PATH unset (--standalone single container): assign
    //     null to explicitly disable worker coordination — this image ships
    //     no worker binary and runs HTML-processing-only by design (see
    //     run-demo.sh). This is a genuine "no worker" mode, NOT the
    //     accidental demo-disable that broke BasicWebApp's dashboard.
    options.Worker.SocketPath = socketPath;
});

var app = builder.Build();

app.UseExceptionHandler("/error");

// Bypass: skip PageSpeed when ?bypass=1 is present so users can
// compare optimized vs. unoptimized in DevTools.
app.UseWhen(
    ctx => !ctx.Request.Query.ContainsKey("bypass"),
    branch => branch.UsePageSpeed());

app.UseStaticFiles();
app.UseRouting();
app.MapHealthChecks("/health");

// ── Pages ──────────────────────────────────────────────────────

app.MapGet("/", () => Results.Content(Pages.Home, "text/html"));
app.MapGet("/gallery", () => Results.Content(Pages.Gallery, "text/html"));
app.MapGet("/blog", () => Results.Content(Pages.Blog, "text/html"));
app.MapGet("/about", () => Results.Content(Pages.About, "text/html"));

// API endpoint — excluded by default (/api/ prefix in ExcludePaths),
// so JSON passes through unmodified by the middleware.
app.MapGet("/api/status", () => Results.Json(new
{
    status = "ok",
    pagespeed = new { enabled = true, version = "2.0" },
    features = new[]
    {
        "critical-css", "lazy-load", "lcp-preload",
        "preconnect", "image-dimensions"
    }
}));

app.Run();

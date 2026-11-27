// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using WeAmp.PageSpeed.AspNetCore;

var builder = WebApplication.CreateBuilder(args);

builder.Services.AddPageSpeed(options =>
{
    options.Cache.VolumePath = "/tmp/pagespeed-demo/volume.dat";
    options.Cache.VolumeSizeBytes = 256 * 1024 * 1024;
    // Worker.SocketPath is intentionally left unset: with AutoStart (the
    // default), the middleware auto-resolves a socket path, launches the
    // worker, and starts reporting notifications / variants — so the
    // Dashboard, Savings, and Metrics pages have real data.
});

var app = builder.Build();

// Middleware ordering matters:
app.UseExceptionHandler("/error");
app.UsePageSpeed();          // Before compression and static files
app.UseStaticFiles();
app.UseRouting();
app.MapHealthChecks("/health");

app.MapGet("/", () => Results.Content("""
    <!DOCTYPE html>
    <html>
    <head>
        <title>PageSpeed ASP.NET Core Demo</title>
        <link rel="stylesheet" href="/styles.css">
    </head>
    <body>
        <h1>Hello from PageSpeed + ASP.NET Core</h1>
        <img src="/hero.jpg" width="800" height="400" alt="Hero">
        <p>This HTML was processed by libpagespeed.so via P/Invoke.</p>
    </body>
    </html>
    """, "text/html"));

app.Run();

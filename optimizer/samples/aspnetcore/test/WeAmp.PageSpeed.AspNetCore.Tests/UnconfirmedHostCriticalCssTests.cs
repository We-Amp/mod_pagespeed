// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;
using Xunit.Abstractions;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// An application left at the template's <c>AllowedHosts</c> of <c>*</c>
/// confirms no host name. A stylesheet the package cached for a visitor who
/// was not signed in is still used, so the page the worker optimizes still
/// gets critical CSS from its external stylesheet.
///
/// Full stack: the real middleware and native cache with the auto-started
/// <c>factory_worker</c>. Runs whenever the native assets are present;
/// reported as skipped otherwise, or fails when
/// <c>PAGESPEED_E2E_REQUIRE_NATIVE=1</c>.
/// </summary>
[Collection(WorkerSubprocessCollection.Name)]
public class UnconfirmedHostCriticalCssTests
{
    // The stylesheet sits after the page content, so the worker inlines the
    // above-the-fold CSS whether or not the page has been confirmed for
    // deferral (no browser runs here).
    // A colour no minifier shortens, so it is found as written.
    private const string SheetMarker = "4d5e6f";
    private const string SheetCss = ".hero{color:#" + SheetMarker + "}";

    private const string PageHtml = """
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8">
            <title>Unconfirmed host</title>
            <style>body{margin:0;font-family:sans-serif}</style>
        </head>
        <body>
            <section class="hero"><h1>Hello</h1><p>Above the fold.</p></section>
            <link rel="stylesheet" href="/site.css">
        </body>
        </html>
        """;

    private readonly ITestOutputHelper _output;

    public UnconfirmedHostCriticalCssTests(ITestOutputHelper output)
    {
        _output = output;
    }

    [WorkerFact]
    public async Task AnyHostAllowed_WorkerPageKeepsCriticalCssFromStylesheet()
    {
        Assert.True(NativeAssetsFixture.TryStage(_output, out var skipReason), skipReason);

        var cacheDir = Path.Combine(
            Path.GetTempPath(), $"ps_unconfirmed_{Guid.NewGuid():N}");
        Directory.CreateDirectory(cacheDir);

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);
            // The project template's value: any host, so none is confirmed.
            builder.Configuration["AllowedHosts"] = "*";
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = Path.Combine(cacheDir, "volume.dat");
                opts.Cache.VolumeSizeBytes = 64 * 1024 * 1024;
            });

            await using var app = builder.Build();
            app.UsePageSpeed();
            app.MapGet("/site.css", (HttpContext ctx) =>
            {
                ctx.Response.Headers.CacheControl = "public, max-age=3600";
                return Results.Text(SheetCss, "text/css");
            });
            app.MapGet("/", (HttpContext ctx) =>
            {
                ctx.Response.Headers.CacheControl = "public, max-age=600";
                return Results.Content(PageHtml, "text/html");
            });

            await app.StartAsync();
            try
            {
                var endpoint = app.Services.GetRequiredService<InternalWorkerEndpoint>();
                var apiPort = await endpoint.Ready.WaitAsync(TimeSpan.FromSeconds(30));
                using var workerClient = new HttpClient
                {
                    BaseAddress = new Uri($"http://127.0.0.1:{apiPort}"),
                    Timeout = TimeSpan.FromSeconds(10),
                };
                await WaitForWorkerReadyAsync(workerClient);

                using var client = app.GetTestClient();
                client.BaseAddress = new Uri("https://localhost/");

                // An anonymous visitor fetches the stylesheet, then the page.
                (await client.GetAsync("/site.css")).EnsureSuccessStatusCode();
                var first = await GetPageAsync(client);
                _output.WriteLine($"first response ({first.State}):\n{first.Body}");

                // Wait until the worker has processed the page and written
                // its output.
                var stats = await PollStatsAsync(workerClient, TimeSpan.FromSeconds(60),
                    s => s.Html >= 1 && s.Variants > 0);
                _output.WriteLine($"/v1/stats: {stats}");

                // The worker's output replaces the page the middleware
                // stored; wait (bounded) for the served page to change.
                var served = await GetPageAsync(client);
                var watch = Stopwatch.StartNew();
                while (served.Body == first.Body && watch.Elapsed < TimeSpan.FromSeconds(15))
                {
                    await Task.Delay(250);
                    served = await GetPageAsync(client);
                }
                _output.WriteLine($"served ({served.State}):\n{served.Body}");

                Assert.Equal("HIT", served.State);
                var block = Regex.Match(served.Body,
                    "<style[^>]*data-pagespeed-critical[^>]*>(.*?)</style>",
                    RegexOptions.Singleline);
                Assert.True(block.Success, $"no critical CSS in {served.Body}");
                Assert.Contains(SheetMarker, block.Groups[1].Value);
            }
            finally
            {
                await app.StopAsync();
            }
        }
        finally
        {
            try { Directory.Delete(cacheDir, recursive: true); } catch { }
        }
    }

    private static async Task<(string State, string Body)> GetPageAsync(HttpClient client)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, "/");
        request.Headers.Accept.ParseAdd("text/html");
        using var response = await client.SendAsync(request);
        response.EnsureSuccessStatusCode();
        var state = response.Headers.TryGetValues("X-PageSpeed", out var v)
            ? string.Join(",", v) : "";
        return (state, await response.Content.ReadAsStringAsync());
    }

    private static async Task WaitForWorkerReadyAsync(HttpClient client)
    {
        for (int i = 0; i < 60; i++)
        {
            try
            {
                using var r = await client.GetAsync("/v1/stats");
                if (r.IsSuccessStatusCode) return;
            }
            catch (HttpRequestException) { }
            catch (TaskCanceledException) { }
            await Task.Delay(500);
        }
        throw new TimeoutException(
            $"worker /v1/stats never reached 200 within ~30s on {client.BaseAddress}");
    }

    private readonly record struct StatsSnapshot(long Variants, long Html);

    private static async Task<string> PollStatsAsync(
        HttpClient client, TimeSpan timeout, Func<StatsSnapshot, bool> until)
    {
        var deadline = DateTime.UtcNow + timeout;
        var last = "";
        while (DateTime.UtcNow < deadline)
        {
            using var r = await client.GetAsync("/v1/stats");
            last = await r.Content.ReadAsStringAsync();
            using var doc = JsonDocument.Parse(last);
            var root = doc.RootElement;
            var snap = new StatsSnapshot(
                root.GetProperty("variants").GetProperty("written").GetInt64(),
                root.GetProperty("by_type").GetProperty("html").GetProperty("count").GetInt64());
            if (until(snap)) return last;
            await Task.Delay(500);
        }
        throw new TimeoutException($"worker did not process the page: {last}");
    }
}

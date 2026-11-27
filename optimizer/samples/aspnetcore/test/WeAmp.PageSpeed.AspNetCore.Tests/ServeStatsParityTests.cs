// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Text.Json;
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
/// The port-level data-path gate: PROVES that the in-process
/// ASP.NET middleware writes the shared <c>.pagespeed-serve-stats</c> mmap on
/// worker-processed cache HITs, reaching parity with the nginx 2.0 front-end.
///
/// Before this change the in-process front-end served cache HITs with zero
/// side effects (a pure local mmap read), so the worker's <c>/v1/stats</c>
/// <c>serve_savings.*</c> group — which powers the trial console Dashboard's
/// "Total Bandwidth Saved" hero, the "Cache Hits" tile, the Savings page, and
/// the Metrics CSV/JSON exports — stayed <c>0</c> forever, while the nginx
/// integration populated them correctly.
///
/// FAILING-FIRST baseline: on <c>origin/main</c> (without the
/// <c>ServeCacheHit</c> → <c>RecordServeHit</c> write path) every
/// <c>serve_savings.image.{hits,original_bytes,optimized_bytes}</c> reads 0 and
/// this test fails at the assertions. With the write path it passes.
///
/// FULL-STACK harness, mirroring
/// <see cref="WorkerSocketPathDefaultTests.DefaultConfig_GetDrivesVariantsWritten"/>:
/// it boots a real <see cref="WebApplication"/> with the production default
/// config (worker auto-started by <c>WorkerProcessHost</c>, which pins and
/// publishes the IPC socket the notification service rendezvouses on — the
/// only path where notifications actually reach the worker), the real native
/// cache (P/Invoke into libpagespeed), and a real <c>factory_worker</c>
/// subprocess. <c>await using var app</c> ensures <c>WorkerProcessHost</c>
/// kills the worker child on dispose (no zombie-worker leak on CI runners).
///
/// Inputs (without them the test is reported as skipped, or fails when
/// <c>PAGESPEED_E2E_REQUIRE_NATIVE=1</c>):
/// <list type="bullet">
/// <item><c>factory_worker</c> from <c>PAGESPEED_E2E_NATIVE_DIR</c>, the test
/// output or the restored NuGet native-assets layout, staged via
/// <see cref="NativeAssetsFixture"/>, and <c>libpagespeed</c> in the test
/// output (a build with <c>PageSpeedNativeAssetsDir</c>).</item>
/// </list>
/// </summary>
[Collection(WorkerSubprocessCollection.Name)]
public sealed class ServeStatsParityTests
{
    private readonly ITestOutputHelper _output;

    public ServeStatsParityTests(ITestOutputHelper output)
    {
        _output = output;
    }

    [WorkerFact]
    public async Task WorkerProcessedImageHit_RecordsServeSavings_InSharedMmap()
    {
        Assert.True(NativeAssetsFixture.TryStage(_output, out var skipReason), skipReason);

        var jpegBytes = LoadJpegFixture();
        Assert.True(jpegBytes.Length > 10_000,
            $"fixture too small ({jpegBytes.Length} bytes) to demonstrate savings");

        var cacheDir = Path.Combine(
            Path.GetTempPath(), $"ps_servestats_{Guid.NewGuid():N}");
        Directory.CreateDirectory(cacheDir);
        var cachePath = Path.Combine(cacheDir, "volume.dat");

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);

            // AddPageSpeed's version check P/Invokes libpagespeed; the
            // [WorkerFact] gate only runs this test when it is in place.
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = cachePath;
                opts.Cache.VolumeSizeBytes = 64 * 1024 * 1024;
                // Production default: AutoStart=true, no explicit SocketPath.
                // WorkerProcessHost auto-resolves + publishes the IPC
                // endpoint the notifier rendezvouses on.
            });

            // await using: WorkerProcessHost kills the factory_worker child in
            // Dispose() (not StopAsync), so disposal prevents a worker leak.
            await using var app = builder.Build();
            app.UsePageSpeed();
            app.Run(async ctx =>
            {
                if (ctx.Request.Path == "/test-image.jpg")
                {
                    ctx.Response.ContentType = "image/jpeg";
                    ctx.Response.Headers.CacheControl = "public, max-age=600";
                    await ctx.Response.Body.WriteAsync(jpegBytes);
                    return;
                }
                ctx.Response.StatusCode = 404;
            });

            await app.StartAsync();
            try
            {
                var endpoint = app.Services.GetRequiredService<InternalWorkerEndpoint>();
                var apiPort = await endpoint.Ready.WaitAsync(TimeSpan.FromSeconds(30));
                var pinnedSock = await endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(30));
                Assert.NotNull(pinnedSock);
                _output.WriteLine($"worker API port: {apiPort}, socket: {pinnedSock}");

                using var workerClient = new HttpClient
                {
                    BaseAddress = new Uri($"http://127.0.0.1:{apiPort}"),
                    Timeout = TimeSpan.FromSeconds(10),
                };
                await WaitForWorkerReadyAsync(workerClient);

                // The worker creates {cache_parent}/.pagespeed-serve-stats at
                // startup; confirm the file the middleware will lazily open
                // exists (the precondition for the whole feature).
                var serveStatsPath = Path.Combine(cacheDir, ".pagespeed-serve-stats");
                for (int i = 0; i < 40 && !File.Exists(serveStatsPath); i++)
                    await Task.Delay(250);
                Assert.True(File.Exists(serveStatsPath),
                    $"worker did not create {serveStatsPath}");
                _output.WriteLine($".pagespeed-serve-stats present at {serveStatsPath}");

                var client = app.GetTestClient();

                // Drive WebP/AVIF Accept and bounded-poll until the worker has
                // materialized a SMALLER variant — i.e. the middleware is now
                // serving a worker-processed cache HIT. First poll is a MISS
                // (middleware notifies worker → worker transcodes async);
                // subsequent polls become HITs once the variant lands. Each
                // served HIT runs through ServeCacheHit, recording into the mmap.
                byte[] variantBody = Array.Empty<byte>();
                string? variantCt = null;
                bool variantServed = false;
                // Image transcoding (WebP/AVIF + ssimulacra2 quality search) is
                // CPU-intensive. The 180 s bound covers native assets built
                // without optimization, where it takes over a minute; an
                // optimized build produces the variant within seconds. The
                // worker reports thread_pool.inflight>0 while it's still
                // transcoding; the variant lands once it finishes.
                const int maxPolls = 360;         // ~180s ceiling
                for (int i = 0; i < maxPolls; i++)
                {
                    using var resp = await GetImageAsync(
                        client, "image/webp,image/avif,*/*;q=0.8");
                    Assert.Equal(HttpStatusCode.OK, resp.StatusCode);
                    variantBody = await resp.Content.ReadAsByteArrayAsync();
                    variantCt = resp.Content.Headers.ContentType?.MediaType;
                    var xps = resp.Headers.TryGetValues("X-PageSpeed", out var v)
                        ? string.Join(",", v) : "";

                    if (variantBody.Length < jpegBytes.Length &&
                        (variantCt == "image/webp" || variantCt == "image/avif"))
                    {
                        _output.WriteLine(
                            $"variant served (HIT) after poll #{i + 1}: "
                            + $"{variantBody.Length} bytes, ct={variantCt}, X-PageSpeed={xps}");
                        variantServed = true;
                        // A few more HITs so the counter is unambiguously >= 1
                        // and to exercise the lock-free record path repeatedly.
                        for (int j = 0; j < 3; j++)
                        {
                            using var extra = await GetImageAsync(
                                client, "image/webp,image/avif,*/*;q=0.8");
                            Assert.Equal(HttpStatusCode.OK, extra.StatusCode);
                        }
                        break;
                    }

                    await Task.Delay(500);
                }

                // Dump stats unconditionally (diagnostic on both pass & fail).
                var statsJson = await workerClient.GetStringAsync("/v1/stats");
                _output.WriteLine($"[worker /v1/stats] {statsJson}");

                Assert.True(
                    variantServed,
                    $"worker never served an optimized image variant within ~180s; "
                    + $"cannot exercise the serve-stats write path. last body="
                    + $"{variantBody.Length} bytes, ct={variantCt ?? "<none>"}");

                // ── Assert serve_savings.image from the worker's /v1/stats ──

                using var doc = JsonDocument.Parse(statsJson);
                var root = doc.RootElement;
                Assert.True(
                    root.TryGetProperty("serve_savings", out var serveSavings),
                    "worker /v1/stats has no serve_savings group");
                var image = serveSavings.GetProperty("image");
                var hits = image.GetProperty("hits").GetInt64();
                var originalBytes = image.GetProperty("original_bytes").GetInt64();
                var optimizedBytes = image.GetProperty("optimized_bytes").GetInt64();

                _output.WriteLine(
                    $"serve_savings.image: hits={hits} original_bytes={originalBytes} "
                    + $"optimized_bytes={optimizedBytes}");

                // The failing-first assertions. On origin/main these are all 0.
                Assert.True(hits >= 1,
                    $"serve_savings.image.hits should be >= 1 after a worker-processed "
                    + $"image HIT, got {hits}. The in-process middleware did not record "
                    + $"the serve into the shared mmap (serve-stats regression).");
                Assert.True(originalBytes > 0,
                    $"serve_savings.image.original_bytes should be > 0, got {originalBytes}.");
                Assert.True(optimizedBytes > 0,
                    $"serve_savings.image.optimized_bytes should be > 0, got {optimizedBytes}.");

                // Savings must be real: the optimized variant is smaller than the
                // origin, so optimized_bytes <= original_bytes across the hits.
                Assert.True(originalBytes >= optimizedBytes,
                    $"serve_savings.image.original_bytes ({originalBytes}) should be "
                    + $">= optimized_bytes ({optimizedBytes}); a HIT recorded inverted "
                    + $"savings, which would corrupt the dashboard ratio.");
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

    private static Task<HttpResponseMessage> GetImageAsync(
        HttpClient client, string acceptHeader)
    {
        var req = new HttpRequestMessage(HttpMethod.Get, "/test-image.jpg");
        req.Headers.Accept.Clear();
        foreach (var part in acceptHeader.Split(','))
            req.Headers.Accept.ParseAdd(part.Trim());
        return client.SendAsync(req);
    }

    private async Task WaitForWorkerReadyAsync(HttpClient client)
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

    private static byte[] LoadJpegFixture()
    {
        var envFixture = Environment.GetEnvironmentVariable("PAGESPEED_E2E_JPEG_FIXTURE");
        if (!string.IsNullOrWhiteSpace(envFixture) && File.Exists(envFixture))
            return File.ReadAllBytes(envFixture);

        var bundled = Path.Combine(
            AppContext.BaseDirectory, "TestFixtures", "test-image.jpg");
        if (File.Exists(bundled)) return File.ReadAllBytes(bundled);

        var probe = AppContext.BaseDirectory;
        for (int i = 0; i < 8 && probe is not null; i++)
        {
            foreach (var rel in new[]
            {
                Path.Combine("samples", "DemoSite", "wwwroot", "img", "hero.jpg"),
                Path.Combine("samples", "DemoSite", "wwwroot", "img", "photo-1.jpg"),
            })
            {
                var candidate = Path.Combine(probe, rel);
                if (File.Exists(candidate)) return File.ReadAllBytes(candidate);
            }
            probe = Directory.GetParent(probe)?.FullName;
        }

        throw new FileNotFoundException(
            "JPEG fixture not found. Set PAGESPEED_E2E_JPEG_FIXTURE to a .jpg, "
            + "or ensure TestFixtures/test-image.jpg ships to the test output.");
    }
}

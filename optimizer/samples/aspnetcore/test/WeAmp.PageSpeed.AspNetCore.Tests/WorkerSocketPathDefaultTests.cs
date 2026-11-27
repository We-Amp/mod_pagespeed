// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net.Http.Json;
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
/// The regression gate for the v2.0.14 "dead trial experience" bug.
///
/// The reference sample shipped <c>options.Worker.SocketPath = null;</c> with
/// the comment <c>// No worker in demo mode</c>, and
/// <see cref="WorkerOptions.SocketPath"/> defaulted to <c>null</c>. With a
/// null socket path, <see cref="WorkerNotificationService"/> silently drops
/// every notification (<c>if (socketPath == null) continue;</c>), so the
/// worker's <c>/v1/stats</c> reports <c>notifications.received == 0</c>,
/// <c>variants.written == 0</c>, and every <c>by_type.*</c> count stays 0
/// forever. Customers conclude the Dashboard / Savings / Metrics pages — and
/// therefore the product — are broken.
///
/// This test drives the FULL middleware host with the production default
/// (no explicit SocketPath set anywhere) against a single <c>GET /</c> that
/// returns HTML, then reads the worker's own <c>/v1/stats</c> via the
/// loopback API port that <see cref="WorkerProcessHost"/> allocates and
/// publishes through <see cref="InternalWorkerEndpoint"/>. It asserts the
/// counters actually move.
///
/// On the buggy default (SocketPath == null) this fails at
/// <c>notifications.received == 0</c>. With the auto-path default it passes.
///
/// Native inputs (without them the tests are reported as skipped, or fail
/// when <c>PAGESPEED_E2E_REQUIRE_NATIVE=1</c>):
/// <list type="bullet">
/// <item><c>PAGESPEED_E2E_NATIVE_DIR</c>: a directory containing both
/// <c>libpagespeed.{dylib,so,dll}</c> and <c>factory_worker[.exe]</c>.</item>
/// <item>Otherwise the test output's <c>runtimes/&lt;rid&gt;/native/</c>,
/// where a build with <c>PageSpeedNativeAssetsDir</c> copies them.</item>
/// <item>Otherwise the restored NuGet native-assets layout under
/// <c>~/.nuget/packages/weamp.pagespeed.nativeassets.*</c> for the current
/// RID is probed.</item>
/// </list>
/// The native assets are staged into the test's
/// <c>runtimes/&lt;rid&gt;/native/</c> so both the P/Invoke resolver
/// (<c>PageSpeedLibrary</c>) and <c>WorkerProcessHost.ResolveWorkerPath</c>
/// find them at the path they expect.
/// </summary>
[Collection(WorkerSubprocessCollection.Name)]
public class WorkerSocketPathDefaultTests
{
    private readonly ITestOutputHelper _output;

    public WorkerSocketPathDefaultTests(ITestOutputHelper output)
    {
        _output = output;
    }

    /// <summary>
    /// The core regression gate, and the one that runs whenever the native
    /// assets are present: with the production default — NO explicit
    /// Worker.SocketPath — the auto-started worker and the notification
    /// service must rendezvous on the SAME endpoint, so a single notification
    /// reaches the worker and <c>notifications.received</c> ticks to 1.
    ///
    /// This deliberately drives <see cref="WorkerNotificationService.TryNotify"/>
    /// directly rather than through a <c>GET /</c>, so it isolates the
    /// socket-path default from the rest of the optimization pipeline (which
    /// <see cref="DefaultConfig_GetDrivesVariantsWritten"/> covers end to end).
    ///
    /// On the buggy default (SocketPath == null) the notification service drops
    /// every notification, so received stays 0 and this fails.
    /// </summary>
    [WorkerFact]
    public async Task DefaultConfig_NoExplicitSocketPath_WorkerReceivesNotification()
    {
        Assert.True(NativeAssetsFixture.TryStage(_output, out var skipReason), skipReason);

        var cacheDir = Path.Combine(
            Path.GetTempPath(), $"ps_sockdefault_{Guid.NewGuid():N}");
        Directory.CreateDirectory(cacheDir);
        var cachePath = Path.Combine(cacheDir, "volume.dat");

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);

            // The production default path: NO Worker.SocketPath is set.
            // AutoStart is true by default.
            // AddPageSpeed's version check P/Invokes libpagespeed; the
            // [WorkerFact] gate only runs this test when it is in place.
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = cachePath;
                opts.Cache.VolumeSizeBytes = 64 * 1024 * 1024;
                // Intentionally DO NOT set opts.Worker.SocketPath.
            });

            // `await using` so the host is disposed on scope exit:
            // WorkerProcessHost kills the factory_worker child in Dispose()
            // (not StopAsync), so without disposal the worker leaks — which
            // accumulates zombie workers on the persistent CI runners.
            await using var app = builder.Build();
            app.UsePageSpeed();

            await app.StartAsync();
            try
            {
                var endpoint = app.Services.GetRequiredService<InternalWorkerEndpoint>();
                var apiPort = await endpoint.Ready.WaitAsync(TimeSpan.FromSeconds(30));
                var pinnedSock = await endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(30));
                _output.WriteLine($"worker API port: {apiPort}");
                _output.WriteLine($"pinned socket path: {pinnedSock ?? "<null>"}");

                // The default must NOT be the disabled (null) state.
                Assert.NotNull(pinnedSock);

                using var workerClient = new HttpClient
                {
                    BaseAddress = new Uri($"http://127.0.0.1:{apiPort}"),
                    Timeout = TimeSpan.FromSeconds(10),
                };
                await WaitForWorkerReadyAsync(workerClient);

                // Fire one notification through the real, fully-wired
                // notification service. With the buggy null default this is
                // silently dropped; with the auto-path default it lands.
                var notifier =
                    app.Services.GetRequiredService<WorkerNotificationService>();
                Assert.True(notifier.TryNotify(
                    "/index.html", "localhost", "https",
                    PageSpeedContentType.Html, 0x08));

                var (received, _, _, lastStats) =
                    await PollStatsAsync(
                        workerClient, TimeSpan.FromSeconds(20),
                        until: s => s.Received >= 1);

                _output.WriteLine($"final /v1/stats: {lastStats}");
                Assert.Equal(1, received);
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

    /// <summary>
    /// Full end-to-end through <c>GET /</c>: with the default config the
    /// middleware optimizes the HTML, caches it, notifies the worker, and the
    /// worker writes at least one variant — so the console's Dashboard /
    /// Savings / Metrics pages light up. Runs whenever the native assets are
    /// present; reported as skipped otherwise.
    /// </summary>
    [WorkerFact]
    public async Task DefaultConfig_GetDrivesVariantsWritten()
    {
        Assert.True(NativeAssetsFixture.TryStage(_output, out var skipReason), skipReason);

        var cacheDir = Path.Combine(
            Path.GetTempPath(), $"ps_sockdefault_lic_{Guid.NewGuid():N}");
        Directory.CreateDirectory(cacheDir);
        var cachePath = Path.Combine(cacheDir, "volume.dat");

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);

            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = cachePath;
                opts.Cache.VolumeSizeBytes = 64 * 1024 * 1024;
                // Intentionally DO NOT set opts.Worker.SocketPath.
            });

            // `await using`: dispose the host on scope exit so WorkerProcessHost
            // kills the factory_worker child (it does so in Dispose(), not
            // StopAsync) — otherwise workers leak on the CI runners.
            await using var app = builder.Build();
            app.UsePageSpeed();
            app.MapGet("/", () => Results.Content(SampleHtml, "text/html"));

            await app.StartAsync();
            try
            {
                var endpoint = app.Services.GetRequiredService<InternalWorkerEndpoint>();
                var apiPort = await endpoint.Ready.WaitAsync(TimeSpan.FromSeconds(30));
                var pinnedSock = await endpoint.SocketPathReady.WaitAsync(TimeSpan.FromSeconds(30));
                Assert.NotNull(pinnedSock);

                using var workerClient = new HttpClient
                {
                    BaseAddress = new Uri($"http://127.0.0.1:{apiPort}"),
                    Timeout = TimeSpan.FromSeconds(10),
                };
                await WaitForWorkerReadyAsync(workerClient);

                using var testClient = app.GetTestClient();
                using var resp = await testClient.GetAsync("/");
                resp.EnsureSuccessStatusCode();
                var body = await resp.Content.ReadAsStringAsync();
                Assert.Contains("<html", body, StringComparison.OrdinalIgnoreCase);

                var (received, variantsWritten, htmlCount, lastStats) =
                    await PollStatsAsync(
                        workerClient, TimeSpan.FromSeconds(20),
                        until: s => s.Received >= 1 && s.Variants > 0 && s.Html >= 1);

                _output.WriteLine($"final /v1/stats: {lastStats}");

                Assert.Equal(1, received);
                Assert.True(
                    variantsWritten > 0,
                    $"expected variants.written > 0, got {variantsWritten}. stats={lastStats}");
                Assert.True(
                    htmlCount >= 1,
                    $"expected by_type.html.count >= 1, got {htmlCount}. stats={lastStats}");
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

    private readonly record struct StatsSnapshot(int Received, long Variants, long Html);

    private async Task<(int Received, long VariantsWritten, long HtmlCount, string LastStats)>
        PollStatsAsync(HttpClient client, TimeSpan timeout, Func<StatsSnapshot, bool> until)
    {
        var deadline = DateTime.UtcNow + timeout;
        string last = "";
        var snap = new StatsSnapshot(0, 0, 0);
        while (DateTime.UtcNow < deadline)
        {
            using var r = await client.GetAsync("/v1/stats");
            last = await r.Content.ReadAsStringAsync();
            using var doc = JsonDocument.Parse(last);
            var root = doc.RootElement;
            snap = new StatsSnapshot(
                root.GetProperty("notifications").GetProperty("received").GetInt32(),
                root.GetProperty("variants").GetProperty("written").GetInt64(),
                root.GetProperty("by_type").GetProperty("html").GetProperty("count").GetInt64());
            if (until(snap))
                break;
            await Task.Delay(500);
        }
        return (snap.Received, snap.Variants, snap.Html, last);
    }

    // A non-trivial HTML doc with a stylesheet + image reference so the worker
    // has real optimization work to do (critical CSS, image dimensions, etc.)
    // and therefore writes at least one variant.
    private const string SampleHtml = """
        <!DOCTYPE html>
        <html lang="en">
        <head>
            <meta charset="utf-8">
            <title>Trial Experience Smoke</title>
            <style>
                body { margin: 0; font-family: system-ui, sans-serif; color: #111; }
                .hero { padding: 4rem 2rem; background: #f5f5f5; }
                h1 { font-size: 2.5rem; line-height: 1.1; }
                p { font-size: 1.125rem; max-width: 40rem; }
            </style>
        </head>
        <body>
            <section class="hero">
                <h1>Hello from PageSpeed + ASP.NET Core</h1>
                <p>This HTML response is optimized by libpagespeed via P/Invoke,
                   and the worker is notified to produce cached variants.</p>
            </section>
        </body>
        </html>
        """;
}

/// <summary>
/// Stages the <c>factory_worker</c> binary into the test base directory's
/// <c>runtimes/&lt;rid&gt;/native/</c> so <c>WorkerProcessHost</c> can launch it.
/// <para>
/// Deliberately does NOT stage <c>libpagespeed</c>: placing it in
/// <see cref="AppContext.BaseDirectory"/> makes <c>PageSpeedVersion.Current</c>
/// succeed process-wide, which would otherwise affect <c>HealthCheckTests</c>'
/// native-lib-missing case (same test assembly; that test also skips itself
/// through <see cref="NativeLibAbsentFactAttribute"/> whenever the library is
/// loadable). Tests that call
/// <c>AddPageSpeed</c> still need the library for its version check, so they
/// run only when it is already in that directory (a build with
/// <c>PageSpeedNativeAssetsDir</c> copies it there) and are reported as
/// skipped otherwise; see <see cref="WorkerFactAttribute"/>. Staging only the
/// worker is inert for every other test.
/// </para>
/// </summary>
internal static class NativeAssetsFixture
{
    private static readonly object Gate = new();
    private static bool _workerStaged;

    /// <summary>
    /// Why the worker tests cannot run here, or null when they can: the
    /// worker binary must be found (see <see cref="ResolveSourceDir"/>) and
    /// the native library must be in the test output's
    /// <c>runtimes/&lt;rid&gt;/native/</c>, where the P/Invoke resolver
    /// loads it from.
    /// </summary>
    public static string? WhyNotRunnable()
    {
        var rid = RuntimeInformation.RuntimeIdentifier;
        var workerName = WorkerFileName();
        if (ResolveSourceDir(rid, workerName) == null)
            return $"{workerName} not found for rid={rid}. Set PAGESPEED_E2E_NATIVE_DIR "
                + $"to a directory containing {workerName}, build with "
                + "PageSpeedNativeAssetsDir, or restore the "
                + "WeAmp.PageSpeed.NativeAssets.* package.";
        if (!NativeLibPresent())
            return $"{NativeLibFileName()} not found in {NativeLibDir()}. Build with "
                + "PageSpeedNativeAssetsDir pointing at the native assets.";
        return null;
    }

    /// <summary>
    /// The test output directory the P/Invoke resolver loads the native
    /// library from.
    /// </summary>
    private static string NativeLibDir() => Path.Combine(
        AppContext.BaseDirectory, "runtimes", RuntimeInformation.RuntimeIdentifier, "native");

    /// <summary>Whether the native library is in <see cref="NativeLibDir"/>.</summary>
    public static bool NativeLibPresent() =>
        File.Exists(Path.Combine(NativeLibDir(), NativeLibFileName()));

    public static bool TryStage(ITestOutputHelper output, out string skipReason)
    {
        skipReason = "";
        lock (Gate)
        {
            var rid = RuntimeInformation.RuntimeIdentifier;
            var workerName = WorkerFileName();

            var sourceDir = ResolveSourceDir(rid, workerName);
            if (sourceDir == null)
            {
                skipReason =
                    $"native assets not found for rid={rid}. Set PAGESPEED_E2E_NATIVE_DIR " +
                    $"to a directory containing {workerName}, or restore the " +
                    "WeAmp.PageSpeed.NativeAssets.* package.";
                return false;
            }

            var destDir = Path.Combine(
                AppContext.BaseDirectory, "runtimes", rid, "native");
            Directory.CreateDirectory(destDir);

            if (!_workerStaged &&
                !string.Equals(Path.GetFullPath(sourceDir).TrimEnd(Path.DirectorySeparatorChar),
                    Path.GetFullPath(destDir).TrimEnd(Path.DirectorySeparatorChar),
                    StringComparison.Ordinal))
            {
                CopyIfNewer(Path.Combine(sourceDir, workerName),
                    Path.Combine(destDir, workerName));
                if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                    TryChmodExec(Path.Combine(destDir, workerName));
            }

            _workerStaged = true;
            output.WriteLine($"staged factory_worker from {sourceDir} -> {destDir}");
            return true;
        }
    }

    private static string? ResolveSourceDir(string rid, string workerName)
    {
        bool HasNeeded(string dir) =>
            Directory.Exists(dir) && File.Exists(Path.Combine(dir, workerName));

        var env = Environment.GetEnvironmentVariable("PAGESPEED_E2E_NATIVE_DIR");
        if (!string.IsNullOrWhiteSpace(env) && HasNeeded(env))
            return env;

        // The test output itself: a build with PageSpeedNativeAssetsDir copies
        // the native assets into runtimes/<rid>/native/.
        var output = Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native");
        if (HasNeeded(output))
            return output;

        // Probe the restored NuGet native-assets package for this RID.
        var home = Environment.GetEnvironmentVariable("HOME")
            ?? Environment.GetEnvironmentVariable("USERPROFILE");
        if (!string.IsNullOrEmpty(home))
        {
            var pkgRoot = Path.Combine(home, ".nuget", "packages");
            foreach (var os in new[] { "macos", "linux", "windows" })
            {
                var pkgDir = Path.Combine(
                    pkgRoot, $"weamp.pagespeed.nativeassets.{os}");
                if (!Directory.Exists(pkgDir)) continue;
                // Newest version first.
                foreach (var versionDir in Directory.EnumerateDirectories(pkgDir)
                             .OrderByDescending(d => d))
                {
                    var nativeDir = Path.Combine(
                        versionDir, "runtimes", rid, "native");
                    if (HasNeeded(nativeDir))
                        return nativeDir;
                }
            }
        }

        return null;
    }

    private static void CopyIfNewer(string src, string dest)
    {
        if (!File.Exists(dest)
            || new FileInfo(src).Length != new FileInfo(dest).Length)
        {
            File.Copy(src, dest, overwrite: true);
        }
    }

    [System.Runtime.Versioning.UnsupportedOSPlatform("windows")]
    private static void TryChmodExec(string path)
    {
        try
        {
            var mode = File.GetUnixFileMode(path);
            File.SetUnixFileMode(path, mode
                | UnixFileMode.UserExecute
                | UnixFileMode.GroupExecute
                | UnixFileMode.OtherExecute);
        }
        catch { /* best effort */ }
    }

    private static string WorkerFileName() =>
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? "factory_worker.exe" : "factory_worker";

    private static string NativeLibFileName() =>
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "pagespeed.dll"
        : RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "libpagespeed.dylib"
        : "libpagespeed.so";
}

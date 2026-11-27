// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Diagnostics.HealthChecks;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// PageSpeed:Enabled false at startup opens no cache volume and launches no
/// worker; the first time a hot reload sets it to true, PageSpeed activates:
/// the cache volume is opened, the worker is launched, and the next HTML
/// request is optimized. Switching it off and on again keeps the one cache
/// and the one worker.
/// </summary>
public class HotReloadActivationTests : IDisposable
{
    private const string PageHtml =
        "<!doctype html><html><head><title>t</title>"
        + "<link rel=\"stylesheet\" href=\"/site.css\"></head><body>"
        + "<p>hello</p>"
        + "<img src=\"/a.jpg\"><img src=\"/b.jpg\"><img src=\"/c.jpg\">"
        + "<img src=\"/d.jpg\"><img src=\"/e.jpg\"><img src=\"/f.jpg\">"
        + "</body></html>";

    private readonly string _baseDir =
        Path.Combine(Path.GetTempPath(), $"ps_reload_{Guid.NewGuid():N}");

    private string CacheDir => Path.Combine(_baseDir, "deep");
    private string VolumePath => Path.Combine(CacheDir, "volume.dat");

    public void Dispose()
    {
        try { Directory.Delete(_baseDir, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    [NativeLibFact]
    public async Task EnabledAtRuntime_OpensCacheAndOptimizesTheFirstHtmlRequest()
    {
        var log = new ListLoggerProvider();
        await using var app = BuildApp(log, autoStart: false);
        await app.StartAsync();
        try
        {
            using var client = app.GetTestClient();

            // Disabled: passed through, and nothing on disk.
            var before = await GetPageAsync(client);
            Assert.Equal("", before.State);
            Assert.False(Directory.Exists(CacheDir));
            Assert.False(File.Exists(VolumePath));

            SetEnabled(app, true);

            // The reload opened the cache volume.
            await WaitFor(() => Directory.Exists(CacheDir), "the cache directory");

            var after = await GetPageAsync(client);
            Assert.NotEqual("", after.State);
            Assert.NotEqual(PageHtml, after.Body);
            Assert.DoesNotContain(log.Entries, e => e.Level >= LogLevel.Warning);
        }
        finally
        {
            await app.StopAsync();
        }
    }

    [NativeLibFact]
    public async Task EnableDisableEnable_KeepsOneWorkerAndOneCache()
    {
        var log = new ListLoggerProvider();
        var sentinel = Path.Combine(_baseDir, "launches.txt");
        Directory.CreateDirectory(_baseDir);
        var launcher = WriteLauncher(sentinel);

        await using var app = BuildApp(log, autoStart: true);
        app.Services.GetRequiredService<WorkerProcessHost>().WorkerPathOverride = launcher;
        await app.StartAsync();
        try
        {
            var cache = Assert.IsType<DeferredPageSpeedCache>(
                app.Services.GetRequiredService<IPageSpeedCache>());
            var health = new PageSpeedHealthCheck(app.Services);

            await Task.Delay(300);
            Assert.False(File.Exists(sentinel));
            Assert.False(Directory.Exists(CacheDir));
            Assert.Null(cache.ActiveCache);
            var disabled = await health.CheckHealthAsync(new HealthCheckContext());
            Assert.Equal(HealthStatus.Healthy, disabled.Status);
            Assert.Equal("PageSpeed is disabled", disabled.Description);

            SetEnabled(app, true);
            await WaitFor(() => LaunchCount(sentinel) == 1, "the worker to launch");
            var opened = cache.ActiveCache;
            Assert.NotNull(opened);
            Assert.True(Directory.Exists(CacheDir));
            var enabled = await health.CheckHealthAsync(new HealthCheckContext());
            Assert.Equal("PageSpeed cache is operational", enabled.Description);

            SetEnabled(app, false);
            await Task.Delay(300);
            SetEnabled(app, true);
            await Task.Delay(500);

            Assert.Same(opened, cache.ActiveCache);
            Assert.Equal(1, LaunchCount(sentinel));
            Assert.Single(log.Entries, e => e.Message.Contains("opened the cache volume"));
            Assert.Single(log.Entries, e => e.Message.Contains("Starting factory_worker"));
            Assert.DoesNotContain(log.Entries, e => e.Message.Contains("stopping with the host"));
        }
        finally
        {
            await app.StopAsync();
            File.Delete(launcher);
        }
    }

    [NativeLibFact]
    public async Task EnabledAtRuntime_UnopenableVolume_IsUnhealthyWarnsOnceAndKeepsServing()
    {
        // A regular file where the cache directory should be: the volume
        // cannot be created under it.
        Directory.CreateDirectory(_baseDir);
        var blocker = Path.Combine(_baseDir, "blocker");
        File.WriteAllText(blocker, "not a directory");
        var log = new ListLoggerProvider();
        await using var app = BuildApp(
            log, autoStart: false, volumePath: Path.Combine(blocker, "volume.dat"));
        await app.StartAsync();
        try
        {
            using var client = app.GetTestClient();
            var health = new PageSpeedHealthCheck(app.Services);
            Assert.Equal("PageSpeed is disabled",
                (await health.CheckHealthAsync(new HealthCheckContext())).Description);

            SetEnabled(app, true);

            var result = await health.CheckHealthAsync(new HealthCheckContext());
            Assert.Equal(HealthStatus.Unhealthy, result.Status);
            Assert.Equal("PageSpeed cache could not be opened", result.Description);

            // Pages are still served (optimized inline, not cached), and the
            // skipped cache writes are reported once, not per response.
            for (int i = 0; i < 3; i++)
            {
                var page = await GetPageAsync(client);
                Assert.Contains("hello", page.Body);
            }
            var warning = Assert.Single(log.Entries, e => e.Level == LogLevel.Warning);
            Assert.Contains("cache is not open", warning.Message);
            Assert.Single(log.Entries, e => e.Level >= LogLevel.Error
                && e.Message.Contains("opening the cache volume failed"));
        }
        finally
        {
            await app.StopAsync();
        }
    }

    [NativeLibFact]
    public void HtmlProcessor_BeforeActivation_RunsWithoutACache()
    {
        var monitor = new TestOptionsMonitor(new PageSpeedOptions { Enabled = false });
        var log = new ListLoggerProvider();
        using var activation = new PageSpeedActivation(
            monitor,
            _ => throw new InvalidOperationException("must not open while disabled"),
            log.CreateLogger("activation"));
        var processor = new HtmlProcessor(activation.Cache, monitor);

        using var result = processor.Process(
            System.Text.Encoding.UTF8.GetBytes(PageHtml),
            "https://example.test/", "example.test", "https");

        Assert.Null(activation.ActiveCache);
        Assert.False(activation.Activated.IsCompleted);
        Assert.DoesNotContain(log.Entries, e => e.Level >= LogLevel.Warning);
    }

    private WebApplication BuildApp(
        ListLoggerProvider log, bool autoStart, string? volumePath = null)
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.WebHost.UseTestServer();
        builder.Logging.ClearProviders();
        builder.Logging.SetMinimumLevel(LogLevel.Information);
        builder.Logging.AddProvider(log);
        builder.Configuration.AddInMemoryCollection(new Dictionary<string, string?>
        {
            ["PageSpeed:Enabled"] = "false",
        });
        builder.Services.AddPageSpeed(opts =>
        {
            opts.Cache.VolumePath = volumePath ?? VolumePath;
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = autoStart;
            // With AutoStart off, an unset SocketPath means no worker to
            // notify; with it on, the launched stand-in gets this path.
            if (autoStart)
                opts.Worker.SocketPath = Path.Combine(_baseDir, "worker.sock");
        });

        var app = builder.Build();
        app.UsePageSpeed();
        app.MapGet("/", (HttpContext ctx) =>
        {
            ctx.Response.Headers.CacheControl = "public, max-age=600";
            return Results.Content(PageHtml, "text/html");
        });
        return app;
    }

    /// <summary>Changes PageSpeed:Enabled the way an edited appsettings.json does.</summary>
    private static void SetEnabled(WebApplication app, bool enabled)
    {
        app.Configuration["PageSpeed:Enabled"] = enabled ? "true" : "false";
        ((IConfigurationRoot)app.Configuration).Reload();
    }

    private static async Task<(string State, string Body)> GetPageAsync(HttpClient client)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, "https://localhost/");
        request.Headers.Accept.ParseAdd("text/html");
        using var response = await client.SendAsync(request);
        response.EnsureSuccessStatusCode();
        var state = response.Headers.TryGetValues("X-PageSpeed", out var v)
            ? string.Join(",", v) : "";
        return (state, await response.Content.ReadAsStringAsync());
    }

    private static int LaunchCount(string sentinel) =>
        File.Exists(sentinel) ? File.ReadAllLines(sentinel).Length : 0;

    private static async Task WaitFor(Func<bool> condition, string what)
    {
        var deadline = DateTime.UtcNow.AddSeconds(30);
        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, $"timed out waiting for {what}");
            await Task.Delay(25);
        }
    }

    /// <summary>
    /// A stand-in worker that appends one line per launch to
    /// <paramref name="sentinel"/> and stays up until the host stops it.
    /// </summary>
    private static string WriteLauncher(string sentinel)
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            var cmd = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.cmd");
            File.WriteAllText(cmd,
                $"@echo launched>> \"{sentinel}\"\r\n"
                + "@ping -n 31 127.0.0.1 > nul\r\n");
            return cmd;
        }

        var sh = Path.Combine(Path.GetTempPath(), $"ps-worker-{Guid.NewGuid():N}.sh");
        File.WriteAllText(sh,
            "#!/bin/sh\n"
            + $"echo launched >> '{sentinel}'\n"
            + "exec sleep 30\n");
        File.SetUnixFileMode(sh,
            UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        return sh;
    }

    private sealed class TestOptionsMonitor(PageSpeedOptions value)
        : IOptionsMonitor<PageSpeedOptions>
    {
        public PageSpeedOptions CurrentValue => value;
        public PageSpeedOptions Get(string? name) => value;
        public IDisposable? OnChange(Action<PageSpeedOptions, string?> listener) => null;
    }

    private sealed class ListLoggerProvider : ILoggerProvider
    {
        private readonly List<(LogLevel Level, string Category, string Message)> _entries = new();

        public IReadOnlyList<(LogLevel Level, string Category, string Message)> Entries
        {
            get { lock (_entries) return _entries.ToArray(); }
        }

        public ILogger CreateLogger(string categoryName) => new Logger(this, categoryName);

        public void Dispose()
        {
        }

        private sealed class Logger(ListLoggerProvider owner, string category) : ILogger
        {
            public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
            public bool IsEnabled(LogLevel logLevel) => true;

            public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
                Exception? exception, Func<TState, Exception?, string> formatter)
            {
                lock (owner._entries)
                    owner._entries.Add((logLevel, category, formatter(state, exception)));
            }
        }
    }
}

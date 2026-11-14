// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Integration tests for <see cref="ConsoleMiddleware"/>.
/// We deliberately bypass <c>AddPageSpeed()</c> (which calls
/// <c>VersionCheck.EnsureCompatible()</c> requiring the native lib) and
/// wire only the middleware's dependencies by hand. That keeps these
/// tests green on machines without the C++ engine built — exactly the
/// situation on the Mac dev box at the time of writing.
/// </summary>
public class ConsoleMiddlewareTests : IAsyncLifetime
{
    private IHost? _workerStub;
    private int _workerPort;
    private readonly List<TestRequest> _stubRequests = new();
    private Func<HttpContext, Task>? _stubHandler;

    public async Task InitializeAsync()
    {
        // Find a free loopback port for the worker stub.
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        _workerPort = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();

        _workerStub = Host.CreateDefaultBuilder()
            .ConfigureWebHostDefaults(web =>
            {
                web.ConfigureKestrel(k =>
                {
                    k.Listen(IPAddress.Loopback, _workerPort);
                });
                web.ConfigureLogging(b => b.SetMinimumLevel(LogLevel.Warning));
                web.Configure(app =>
                {
                    app.Run(async ctx =>
                    {
                        var ms = new MemoryStream();
                        await ctx.Request.Body.CopyToAsync(ms);
                        var bodyBytes = ms.ToArray();
                        ctx.Request.Body = new MemoryStream(bodyBytes);

                        _stubRequests.Add(new TestRequest(
                            ctx.Request.Method,
                            ctx.Request.Path + ctx.Request.QueryString,
                            ctx.Request.Headers
                                .ToDictionary(kv => kv.Key, kv => kv.Value.ToString()),
                            bodyBytes));

                        if (_stubHandler != null)
                        {
                            await _stubHandler(ctx);
                            return;
                        }
                        ctx.Response.StatusCode = 200;
                        ctx.Response.ContentType = "application/json";
                        await ctx.Response.WriteAsync("{\"ok\":true}");
                    });
                });
            })
            .Build();

        await _workerStub.StartAsync();
    }

    public async Task DisposeAsync()
    {
        if (_workerStub != null)
        {
            await _workerStub.StopAsync();
            _workerStub.Dispose();
        }
    }

    private record TestRequest(
        string Method,
        string PathAndQuery,
        Dictionary<string, string> Headers,
        byte[] Body);

    /// <summary>
    /// Build a TestServer host with only ConsoleMiddleware in the
    /// pipeline. Hands the middleware an <see cref="InternalWorkerEndpoint"/>
    /// pre-seeded with the worker-stub port (unless <paramref name="setWorkerPort"/>
    /// is false).
    /// </summary>
    private async Task<(IHost host, HttpClient client, InternalWorkerEndpoint endpoint)>
        BuildAppAsync(
            Action<PageSpeedOptions>? configure = null,
            bool setWorkerPort = true,
            int? overridePort = null)
    {
        var options = new PageSpeedOptions();
        configure?.Invoke(options);

        var endpoint = new InternalWorkerEndpoint();
        if (setWorkerPort)
        {
            endpoint.SetPort(overridePort ?? _workerPort);
        }

        var builder = Host.CreateDefaultBuilder()
            .ConfigureWebHost(web =>
            {
                web.UseTestServer();
                web.ConfigureLogging(b => b.SetMinimumLevel(LogLevel.Warning));
                web.ConfigureServices(services =>
                {
                    services.AddSingleton<IOptionsMonitor<PageSpeedOptions>>(
                        new StaticOptionsMonitor<PageSpeedOptions>(options));
                    services.AddSingleton(endpoint);
                    services.AddHttpClient("PageSpeedConsoleProxy", c =>
                    {
                        c.Timeout = TimeSpan.FromSeconds(5);
                    });
                });
                web.Configure(app =>
                {
                    app.UseMiddleware<ConsoleMiddleware>();
                    app.Run(ctx =>
                    {
                        ctx.Response.StatusCode = 404;
                        return Task.CompletedTask;
                    });
                });
            });

        var host = builder.Build();
        await host.StartAsync();
        var client = host.GetTestServer().CreateClient();
        return (host, client, endpoint);
    }

    private sealed class StaticOptionsMonitor<T> : IOptionsMonitor<T>
    {
        private readonly T _value;
        public StaticOptionsMonitor(T value) { _value = value; }
        public T CurrentValue => _value;
        public T Get(string? name) => _value;
        public IDisposable? OnChange(Action<T, string?> listener) => null;
    }

    [RealWebShellFact]
    public async Task ConsoleRoot_ServesIndexHtml()
    {
        var (host, client, _) = await BuildAppAsync();
        try
        {
            var response = await client.GetAsync("/console/");
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            Assert.Equal("text/html",
                response.Content.Headers.ContentType?.MediaType);
            Assert.Equal("no-cache",
                response.Headers.CacheControl?.ToString());
            var body = await response.Content.ReadAsStringAsync();
            Assert.Contains("<script", body, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [RealWebShellFact]
    public async Task ConsoleDeepLink_ReturnsIndexHtml_SpaFallback()
    {
        var (host, client, _) = await BuildAppAsync();
        try
        {
            var response = await client.GetAsync("/console/metrics");
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            Assert.Equal("text/html",
                response.Content.Headers.ContentType?.MediaType);
            var body = await response.Content.ReadAsStringAsync();
            Assert.Contains("<script", body, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [RealWebShellFact(NeedsImmutableAssets = true)]
    public async Task ConsoleImmutableAsset_ServesWithLongMaxAge()
    {
        var (host, client, _) = await BuildAppAsync();
        try
        {
            // Discover a real immutable asset path by scraping the
            // index.html the middleware itself serves.
            var indexBody = await (await client.GetAsync("/console/"))
                .Content.ReadAsStringAsync();

            var marker = "/_app/immutable/";
            var idx = indexBody.IndexOf(marker, StringComparison.Ordinal);
            Assert.True(idx >= 0,
                $"Expected /_app/immutable/ reference in served index.html");
            int end = indexBody.IndexOfAny(new[] { '"', '\'' }, idx);
            Assert.True(end > idx);
            var assetPath = indexBody[idx..end];
            var url = "/console" + assetPath;

            var response = await client.GetAsync(url);
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var cacheControl = response.Headers.CacheControl?.ToString() ?? "";
            Assert.Contains("max-age=31536000", cacheControl);
            Assert.Contains("immutable", cacheControl);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [Fact]
    public async Task CachePurge_PostProxiesToWorker_WithBodyAndHeaders()
    {
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync("{\"purged\":1}");
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var body = "{\"url\":\"https://example.com/\"}";
            using var request = new HttpRequestMessage(
                HttpMethod.Post, "/v1/cache/purge")
            {
                Content = new StringContent(
                    body, Encoding.UTF8, "application/json"),
            };
            request.Headers.TryAddWithoutValidation(
                "X-Requested-With", "XMLHttpRequest");

            var response = await client.SendAsync(request);
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var responseBody = await response.Content.ReadAsStringAsync();
            Assert.Contains("purged", responseBody);

            Assert.Single(_stubRequests);
            var captured = _stubRequests[0];
            Assert.Equal("POST", captured.Method);
            Assert.Equal("/v1/cache/purge", captured.PathAndQuery);
            Assert.True(captured.Headers.ContainsKey("X-Requested-With"),
                "X-Requested-With header should propagate to worker");
            Assert.Equal("XMLHttpRequest", captured.Headers["X-Requested-With"]);
            Assert.Contains("application/json",
                captured.Headers.GetValueOrDefault("Content-Type") ?? "");
            Assert.Equal(body, Encoding.UTF8.GetString(captured.Body));
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [Fact]
    public async Task RetiredLicensePath_IsNotProxiedToWorker()
    {
        // /v1/license/* left the proxy allow-list with the license machinery;
        // such a request must fall through to the app (here: nothing, so 404)
        // and must never reach the worker.
        var (host, client, _) = await BuildAppAsync();
        try
        {
            using var request = new HttpRequestMessage(
                HttpMethod.Post, "/v1/license/apply")
            {
                Content = new StringContent(
                    "{}", Encoding.UTF8, "application/json"),
            };
            request.Headers.TryAddWithoutValidation(
                "X-Requested-With", "XMLHttpRequest");

            var response = await client.SendAsync(request);
            Assert.Equal(HttpStatusCode.NotFound, response.StatusCode);
            Assert.Empty(_stubRequests);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [Fact]
    public async Task ExplicitApiPort_IsHonored_AndPublishedToEndpoint()
    {
        // Stand up a second loopback Kestrel host on a different port
        // and configure the middleware to talk to that port. Verifies
        // the InternalWorkerEndpoint port is read per-request and the
        // proxy routes to whichever loopback port the customer chose.
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var altPort = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();

        var altReceived = false;
        var altHost = Host.CreateDefaultBuilder()
            .ConfigureWebHostDefaults(web =>
            {
                web.ConfigureKestrel(k => k.Listen(IPAddress.Loopback, altPort));
                web.ConfigureLogging(b => b.SetMinimumLevel(LogLevel.Warning));
                web.Configure(app =>
                {
                    app.Run(ctx =>
                    {
                        altReceived = true;
                        ctx.Response.StatusCode = 200;
                        ctx.Response.ContentType = "application/json";
                        return ctx.Response.WriteAsync("{\"alt\":true}");
                    });
                });
            })
            .Build();
        await altHost.StartAsync();
        try
        {
            var (host, client, endpoint) = await BuildAppAsync(
                overridePort: altPort);
            try
            {
                Assert.Equal(altPort, endpoint.Port);
                var response = await client.GetAsync("/v1/health");
                Assert.Equal(HttpStatusCode.OK, response.StatusCode);
                Assert.True(altReceived,
                    "Request should have reached the alt-port stub");
            }
            finally
            {
                await host.StopAsync();
                host.Dispose();
            }
        }
        finally
        {
            await altHost.StopAsync();
            altHost.Dispose();
        }
    }

    [Fact]
    public async Task CustomMountPath_ReroutesConsole_DefaultPath404s()
    {
        var (host, client, _) = await BuildAppAsync(opts =>
        {
            opts.Console.MountPath = "/_pagespeed/console";
        });
        try
        {
            var defaultResponse = await client.GetAsync("/console/");
            // The console middleware passes through; the test app's
            // terminating delegate returns 404.
            Assert.Equal(HttpStatusCode.NotFound, defaultResponse.StatusCode);

            var customResponse = await client.GetAsync("/_pagespeed/console/");
            Assert.Equal(HttpStatusCode.OK, customResponse.StatusCode);
            Assert.Equal("text/html",
                customResponse.Content.Headers.ContentType?.MediaType);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [Fact]
    public async Task RequireHttps_BlocksHttpConsoleRequest()
    {
        var (host, client, _) = await BuildAppAsync(opts =>
        {
            opts.Console.RequireHttps = true;
        });
        try
        {
            // The TestServer client speaks plain HTTP by default.
            var response = await client.GetAsync("/console/");
            Assert.Equal(HttpStatusCode.Forbidden, response.StatusCode);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [Fact]
    public async Task ConsoleDisabled_PassesThrough()
    {
        var (host, client, _) = await BuildAppAsync(opts =>
        {
            opts.Console.Enabled = false;
        });
        try
        {
            var response = await client.GetAsync("/console/");
            Assert.Equal(HttpStatusCode.NotFound, response.StatusCode);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }
}

/// <summary>
/// A fact for a console test that needs the real SvelteKit web-shell build
/// embedded in the assembly. Skipped when the embedded <c>index.html</c> is
/// a stub (no script tag, or with <see cref="NeedsImmutableAssets"/> no
/// <c>/_app/immutable/</c> reference).
/// </summary>
public sealed class RealWebShellFactAttribute : FactAttribute
{
    public bool NeedsImmutableAssets
    {
        get => _needsImmutable;
        set { _needsImmutable = value; Evaluate(); }
    }
    private bool _needsImmutable;

    public RealWebShellFactAttribute() { Evaluate(); }

    private void Evaluate()
    {
        var asm = typeof(ConsoleMiddleware).Assembly;
        var name = asm.GetManifestResourceNames()
            .FirstOrDefault(n => n.EndsWith("index.html", StringComparison.Ordinal));
        string? html = null;
        if (name != null)
        {
            using var s = asm.GetManifestResourceStream(name);
            if (s != null) { using var r = new StreamReader(s); html = r.ReadToEnd(); }
        }
        Skip = SkipReasonFor(html, _needsImmutable);
    }

    /// <summary>
    /// The skip reason for the embedded index.html, or null to run the test.
    /// A missing resource (<paramref name="html"/> null) is a broken embed,
    /// not a stub: the test runs and fails.
    /// </summary>
    internal static string? SkipReasonFor(string? html, bool needsImmutable)
    {
        if (html == null) return null;
        if (!html.Contains("<script", StringComparison.OrdinalIgnoreCase))
            return "The embedded web-shell is a stub (no real build); build it with pnpm to run this test.";
        if (needsImmutable && !html.Contains("/_app/immutable/", StringComparison.Ordinal))
            return "The embedded web-shell has no /_app/immutable/ assets; build it with pnpm to run this test.";
        return null;
    }
}

public class RealWebShellFactAttributeTests
{
    [Fact]
    public void MissingResource_DoesNotSkip() =>
        Assert.Null(RealWebShellFactAttribute.SkipReasonFor(null, false));

    [Fact]
    public void StubResource_Skips() =>
        Assert.NotNull(RealWebShellFactAttribute.SkipReasonFor("<html></html>", false));

    [Fact]
    public void RealResource_DoesNotSkip() =>
        Assert.Null(RealWebShellFactAttribute.SkipReasonFor("<script src=\"/_app/immutable/a.js\">", true));

    [Fact]
    public void ScriptWithoutImmutable_SkipsOnlyWhenRequired()
    {
        Assert.Null(RealWebShellFactAttribute.SkipReasonFor("<script>", false));
        Assert.NotNull(RealWebShellFactAttribute.SkipReasonFor("<script>", true));
    }
}

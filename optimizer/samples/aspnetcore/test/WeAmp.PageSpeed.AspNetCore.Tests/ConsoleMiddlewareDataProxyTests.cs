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
/// Integration tests for <see cref="ConsoleMiddleware"/>'s reverse-proxy
/// of the SPA's <em>data</em> endpoints — <c>/v1/cache/*</c>,
/// <c>/v1/stats</c>, <c>/v1/capture/*</c>, <c>/v1/config</c>.
/// <para>
/// v2.0.12 only proxied <c>/v1/license</c>, <c>/v1/health</c> and
/// <c>/v1/ws</c>, so the SPA's Dashboard / Configuration / URLs /
/// Savings / Metrics pages 404'd against the in-process middleware
/// (the Docker demo reaches the worker directly, masking the gap). These
/// tests pin the expanded surface.
/// </para>
/// <para>
/// The proxy is method-agnostic: <see cref="ConsoleMiddleware.ProxyToWorkerAsync"/>
/// forwards GET/POST/PUT/PATCH/DELETE bodies and headers generically, so
/// these tests drive a representative request per logical group and
/// assert (a) the status code is forwarded, (b) the body is forwarded
/// byte-for-byte, and (c) for mutating requests the request body plus the
/// <c>X-Requested-With</c> CSRF header reach the worker stub. The
/// middleware does NOT enforce CSRF itself: the worker is the
/// authoritative gate (src/worker/http_server.cc) and the middleware is a
/// transparent hop that forwards <c>X-Requested-With</c>.
/// </para>
/// <para>
/// Mirrors the harness in <see cref="ConsoleMiddlewareTests"/> — a
/// loopback Kestrel worker stub records each request it sees and replays a
/// caller-supplied handler.
/// </para>
/// </summary>
public class ConsoleMiddlewareDataProxyTests : IAsyncLifetime
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

    private async Task<(IHost host, HttpClient client, InternalWorkerEndpoint endpoint)>
        BuildAppAsync(Action<PageSpeedOptions>? configure = null)
    {
        var options = new PageSpeedOptions();
        configure?.Invoke(options);

        var endpoint = new InternalWorkerEndpoint();
        endpoint.SetPort(_workerPort);

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

    // -------------------------------------------------------------------
    // /v1/stats — GET, read-only (Dashboard / Metrics pages).
    // -------------------------------------------------------------------
    [Fact]
    public async Task Stats_GetProxiesToWorker_BodyAndStatusForwarded()
    {
        const string payload = "{\"requests\":42,\"bytes_saved\":12345}";
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync(payload);
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var response = await client.GetAsync("/v1/stats");
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var body = await response.Content.ReadAsStringAsync();
            Assert.Equal(payload, body);

            Assert.Single(_stubRequests);
            Assert.Equal("GET", _stubRequests[0].Method);
            Assert.Equal("/v1/stats", _stubRequests[0].PathAndQuery);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    // -------------------------------------------------------------------
    // /v1/cache/urls — GET with query string (URLs page). Confirms
    // segment-aware StartsWithSegments matches the sub-path AND that the
    // query string is forwarded intact.
    // -------------------------------------------------------------------
    [Fact]
    public async Task CacheUrls_GetWithQuery_ProxiesSubPathAndQuery()
    {
        const string payload = "{\"urls\":[\"/a\",\"/b\"],\"total\":2}";
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync(payload);
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var response = await client.GetAsync("/v1/cache/urls?limit=50&offset=0");
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var body = await response.Content.ReadAsStringAsync();
            Assert.Equal(payload, body);

            Assert.Single(_stubRequests);
            Assert.Equal("GET", _stubRequests[0].Method);
            Assert.Equal("/v1/cache/urls?limit=50&offset=0",
                _stubRequests[0].PathAndQuery);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    // -------------------------------------------------------------------
    // /v1/cache/purge — POST, mutating. The SPA sends a JSON body and
    // X-Requested-With. Both MUST reach the worker (which gates CSRF);
    // the middleware adds no CSRF check of its own. This is the most
    // security-relevant new prefix.
    // -------------------------------------------------------------------
    [Fact]
    public async Task CachePurge_PostProxies_BodyAndCsrfHeaderReachWorker()
    {
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync("{\"purged\":true}");
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var body = "{\"url\":\"/style.css\",\"hostname\":\"example.com\"}";
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
            Assert.Equal("{\"purged\":true}", responseBody);

            Assert.Single(_stubRequests);
            var captured = _stubRequests[0];
            Assert.Equal("POST", captured.Method);
            Assert.Equal("/v1/cache/purge", captured.PathAndQuery);
            // (c) request body forwarded byte-for-byte.
            Assert.Equal(body, Encoding.UTF8.GetString(captured.Body));
            // (c) X-Requested-With reached the worker — proves the CSRF
            // header is NOT stripped as hop-by-hop, so the worker's
            // authoritative gate can see it.
            Assert.True(captured.Headers.ContainsKey("X-Requested-With"),
                "X-Requested-With must propagate to the worker CSRF gate");
            Assert.Equal("XMLHttpRequest", captured.Headers["X-Requested-With"]);
            Assert.Contains("application/json",
                captured.Headers.GetValueOrDefault("Content-Type") ?? "");
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    // -------------------------------------------------------------------
    // /v1/cache/purge — middleware does NOT enforce CSRF (that lives in
    // the worker): with no X-Requested-With the worker stub returns 400 and
    // the proxy hands that 400 back verbatim, proving the middleware
    // neither injects the header nor short-circuits.
    // -------------------------------------------------------------------
    [Fact]
    public async Task CachePurge_WithoutCsrfHeader_PassesThroughWorkerStatus()
    {
        _stubHandler = async ctx =>
        {
            if (!ctx.Request.Headers.ContainsKey("X-Requested-With"))
            {
                ctx.Response.StatusCode = 400;
                await ctx.Response.WriteAsync("missing X-Requested-With");
                return;
            }
            ctx.Response.StatusCode = 200;
            await ctx.Response.WriteAsync("ok");
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            using var request = new HttpRequestMessage(
                HttpMethod.Post, "/v1/cache/purge")
            {
                Content = new StringContent(
                    "{}", Encoding.UTF8, "application/json"),
            };

            var response = await client.SendAsync(request);
            Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
            var responseBody = await response.Content.ReadAsStringAsync();
            Assert.Contains("missing X-Requested-With", responseBody);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    // -------------------------------------------------------------------
    // /v1/capture/screenshot — POST, mutating (Savings / capture flow).
    // Body + CSRF header reach the worker.
    // -------------------------------------------------------------------
    [Fact]
    public async Task CaptureScreenshot_PostProxies_BodyAndCsrfHeaderReachWorker()
    {
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 202;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync("{\"job\":\"queued\"}");
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var body = "{\"url\":\"https://example.com/\"}";
            using var request = new HttpRequestMessage(
                HttpMethod.Post, "/v1/capture/screenshot")
            {
                Content = new StringContent(
                    body, Encoding.UTF8, "application/json"),
            };
            request.Headers.TryAddWithoutValidation(
                "X-Requested-With", "XMLHttpRequest");

            var response = await client.SendAsync(request);
            Assert.Equal(HttpStatusCode.Accepted, response.StatusCode);
            var responseBody = await response.Content.ReadAsStringAsync();
            Assert.Equal("{\"job\":\"queued\"}", responseBody);

            Assert.Single(_stubRequests);
            var captured = _stubRequests[0];
            Assert.Equal("POST", captured.Method);
            Assert.Equal("/v1/capture/screenshot", captured.PathAndQuery);
            Assert.Equal(body, Encoding.UTF8.GetString(captured.Body));
            Assert.True(captured.Headers.ContainsKey("X-Requested-With"),
                "X-Requested-With must propagate to the worker CSRF gate");
            Assert.Equal("XMLHttpRequest", captured.Headers["X-Requested-With"]);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    // -------------------------------------------------------------------
    // /v1/config — GET (read) and PATCH (mutate, Configuration page).
    // PATCH exercises a non-POST mutating verb to prove the generic proxy
    // forwards the body + CSRF header for every method, not just POST.
    // -------------------------------------------------------------------
    [Fact]
    public async Task Config_GetProxiesToWorker_BodyAndStatusForwarded()
    {
        const string payload = "{\"jpeg_quality\":85,\"mode\":\"safe\"}";
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync(payload);
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var response = await client.GetAsync("/v1/config");
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var body = await response.Content.ReadAsStringAsync();
            Assert.Equal(payload, body);

            Assert.Single(_stubRequests);
            Assert.Equal("GET", _stubRequests[0].Method);
            Assert.Equal("/v1/config", _stubRequests[0].PathAndQuery);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }

    [Fact]
    public async Task Config_PatchProxies_BodyAndCsrfHeaderReachWorker()
    {
        _stubHandler = async ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            await ctx.Response.WriteAsync("{\"updated\":true}");
        };

        var (host, client, _) = await BuildAppAsync();
        try
        {
            var body = "{\"jpeg_quality\":90}";
            using var request = new HttpRequestMessage(
                HttpMethod.Patch, "/v1/config")
            {
                Content = new StringContent(
                    body, Encoding.UTF8, "application/json"),
            };
            request.Headers.TryAddWithoutValidation(
                "X-Requested-With", "XMLHttpRequest");

            var response = await client.SendAsync(request);
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            var responseBody = await response.Content.ReadAsStringAsync();
            Assert.Equal("{\"updated\":true}", responseBody);

            Assert.Single(_stubRequests);
            var captured = _stubRequests[0];
            Assert.Equal("PATCH", captured.Method);
            Assert.Equal("/v1/config", captured.PathAndQuery);
            Assert.Equal(body, Encoding.UTF8.GetString(captured.Body));
            Assert.True(captured.Headers.ContainsKey("X-Requested-With"),
                "X-Requested-With must propagate to the worker CSRF gate");
            Assert.Equal("XMLHttpRequest", captured.Headers["X-Requested-With"]);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }
}

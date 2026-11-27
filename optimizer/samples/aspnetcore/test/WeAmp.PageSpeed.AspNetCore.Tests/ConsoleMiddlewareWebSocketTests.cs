// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
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
/// Integration tests for <see cref="ConsoleMiddleware"/>'s WebSocket
/// reverse-proxy behavior on the <c>/v1/ws/*</c> prefix.
/// <para>
/// The SPA (debug-console page) opens a WebSocket against
/// <c>/v1/ws/logs</c>, <c>/v1/ws/stats</c>, <c>/v1/ws/events</c>. The
/// worker (factory_worker) terminates these endpoints. The middleware
/// has to bridge the HTTP/1.1 Upgrade handshake from the browser to a
/// loopback WebSocket connection on the worker, and then pump frames
/// in both directions.
/// </para>
/// <para>
/// At the time this test is first introduced the bridge does not
/// exist — the test must fail. Implementation follows in subsequent
/// commits.
/// </para>
/// </summary>
public class ConsoleMiddlewareWebSocketTests : IAsyncLifetime
{
    private IHost? _workerStub;
    private int _workerPort;
    private readonly List<TestWsRequest> _stubRequests = new();
    // Set by individual tests to drive the upstream WS conversation.
    private Func<HttpContext, WebSocket, Task>? _stubWsHandler;

    public async Task InitializeAsync()
    {
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
                    app.UseWebSockets();
                    app.Run(async ctx =>
                    {
                        _stubRequests.Add(new TestWsRequest(
                            ctx.Request.Method,
                            ctx.Request.Path + ctx.Request.QueryString,
                            ctx.Request.Headers
                                .ToDictionary(kv => kv.Key, kv => kv.Value.ToString()),
                            ctx.WebSockets.IsWebSocketRequest));

                        if (!ctx.WebSockets.IsWebSocketRequest)
                        {
                            ctx.Response.StatusCode = 400;
                            await ctx.Response.WriteAsync("expected WebSocket upgrade");
                            return;
                        }

                        using var ws = await ctx.WebSockets.AcceptWebSocketAsync();
                        if (_stubWsHandler != null)
                        {
                            await _stubWsHandler(ctx, ws);
                            return;
                        }
                        // Default: send a single log snapshot then close.
                        var msg = "{\"type\":\"snapshot\",\"entries\":[],\"total\":0}";
                        var bytes = Encoding.UTF8.GetBytes(msg);
                        await ws.SendAsync(
                            new ArraySegment<byte>(bytes),
                            WebSocketMessageType.Text,
                            endOfMessage: true,
                            CancellationToken.None);
                        await ws.CloseAsync(
                            WebSocketCloseStatus.NormalClosure,
                            "bye",
                            CancellationToken.None);
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

    private record TestWsRequest(
        string Method,
        string PathAndQuery,
        Dictionary<string, string> Headers,
        bool IsWebSocketRequest);

    private async Task<(IHost host, TestServer server, InternalWorkerEndpoint endpoint)>
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
                    // ConsoleMiddleware will eventually need WebSockets
                    // available in the pipeline before it. UsePageSpeed()
                    // is expected to wire this on the production code
                    // path. Here we wire it manually to mirror that
                    // contract.
                    app.UseWebSockets();
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
        var server = host.GetTestServer();
        return (host, server, endpoint);
    }

    private sealed class StaticOptionsMonitor<T> : IOptionsMonitor<T>
    {
        private readonly T _value;
        public StaticOptionsMonitor(T value) { _value = value; }
        public T CurrentValue => _value;
        public T Get(string? name) => _value;
        public IDisposable? OnChange(Action<T, string?> listener) => null;
    }

    [Fact]
    public async Task WsLogs_ProxiesUpgrade_AndStreamsFirstMessage()
    {
        // Stub worker emits a /v1/ws/logs snapshot message; the
        // middleware MUST proxy the upgrade and forward the frame.
        _stubWsHandler = async (ctx, ws) =>
        {
            Assert.Equal("/v1/ws/logs", ctx.Request.Path.Value);
            var snapshot =
                "{\"type\":\"log\",\"timestamp\":1,\"source\":\"worker\"," +
                "\"level\":\"info\",\"module\":\"test\",\"message\":\"hello\"}";
            var bytes = Encoding.UTF8.GetBytes(snapshot);
            await ws.SendAsync(
                new ArraySegment<byte>(bytes),
                WebSocketMessageType.Text,
                endOfMessage: true,
                CancellationToken.None);
            // Hold the socket open until the client closes.
            var buf = new byte[256];
            try
            {
                while (ws.State == WebSocketState.Open)
                {
                    var r = await ws.ReceiveAsync(
                        new ArraySegment<byte>(buf),
                        CancellationToken.None);
                    if (r.MessageType == WebSocketMessageType.Close)
                    {
                        await ws.CloseOutputAsync(
                            WebSocketCloseStatus.NormalClosure,
                            "bye",
                            CancellationToken.None);
                        break;
                    }
                }
            }
            catch (WebSocketException) { /* client gone */ }
        };

        var (host, server, _) = await BuildAppAsync();
        try
        {
            var wsClient = server.CreateWebSocketClient();
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            var ws = await wsClient.ConnectAsync(
                new Uri("ws://localhost/v1/ws/logs"),
                cts.Token);

            Assert.Equal(WebSocketState.Open, ws.State);

            var buf = new byte[4096];
            using var recvCts = new CancellationTokenSource(
                TimeSpan.FromSeconds(2));
            var result = await ws.ReceiveAsync(
                new ArraySegment<byte>(buf), recvCts.Token);

            Assert.Equal(WebSocketMessageType.Text, result.MessageType);
            var msg = Encoding.UTF8.GetString(buf, 0, result.Count);
            Assert.Contains("\"type\":\"log\"", msg);
            Assert.Contains("\"source\":\"worker\"", msg);

            // The worker stub MUST have actually seen an upgrade request
            // (i.e. the middleware did not 404 the path locally).
            Assert.Single(_stubRequests);
            Assert.True(_stubRequests[0].IsWebSocketRequest,
                "Worker stub should have received a WebSocket upgrade");
            Assert.Equal("/v1/ws/logs", _stubRequests[0].PathAndQuery);

            await ws.CloseAsync(
                WebSocketCloseStatus.NormalClosure, "done",
                CancellationToken.None);
        }
        finally
        {
            await host.StopAsync();
            host.Dispose();
        }
    }
}

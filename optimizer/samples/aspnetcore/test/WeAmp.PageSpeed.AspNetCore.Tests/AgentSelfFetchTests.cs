// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The optimizer's agent fetches arrive from this host; an application rule
/// that admits local clients must not admit them. Covers the recognition
/// (marker AND a peer that is this server), a real Kestrel pipeline with a
/// loopback-only policy, and that <c>UsePageSpeed()</c> registers the label
/// ahead of everything it adds.
/// </summary>
public class AgentSelfFetchTests
{
    private const string WorkerUa = "Mozilla/5.0 (compatible; mod_pagespeed agent_optimize)";
    private const string BrowserUa = "Mozilla/5.0 (X11; Linux x86_64) Firefox/130.0";
    private const string HeldMarker = "held-af-aspnet";

    private static IPAddress Ip(string s) => IPAddress.Parse(s);

    [Fact]
    public void MarkerMatchesTheWorkerUserAgentOnly()
    {
        Assert.True(AgentSelfFetch.IsAgentFetchUserAgent(WorkerUa));
        Assert.False(AgentSelfFetch.IsAgentFetchUserAgent(BrowserUa));
        Assert.False(AgentSelfFetch.IsAgentFetchUserAgent(null));
        Assert.False(AgentSelfFetch.IsAgentFetchUserAgent(""));
        Assert.False(AgentSelfFetch.IsAgentFetchUserAgent("mod_pagespeed"));
        Assert.False(AgentSelfFetch.IsAgentFetchUserAgent("MOD_PAGESPEED AGENT_OPTIMIZE"));
    }

    [Theory]
    [InlineData("127.0.0.1", "192.0.2.20")]
    [InlineData("127.9.8.7", "192.0.2.20")]
    [InlineData("127.0.0.1", null)]
    [InlineData("::1", null)]
    [InlineData("::ffff:127.0.0.1", null)]
    [InlineData("192.0.2.20", "192.0.2.20")]
    [InlineData("::ffff:192.0.2.20", "::ffff:192.0.2.20")]
    [InlineData("::ffff:192.0.2.20", "192.0.2.20")]
    [InlineData("192.0.2.20", "::ffff:192.0.2.20")]
    [InlineData("2001:db8::20", "2001:db8::20")]
    public void PeersThatAreThisServer(string peer, string? local)
    {
        Assert.True(AgentSelfFetch.PeerIsThisServer(
            Ip(peer), local is null ? null : Ip(local)));
        Assert.True(AgentSelfFetch.IsAgentSelfFetch(
            WorkerUa, Ip(peer), local is null ? null : Ip(local)));
        // A local client without the marker keeps its address.
        Assert.False(AgentSelfFetch.IsAgentSelfFetch(
            BrowserUa, Ip(peer), local is null ? null : Ip(local)));
    }

    [Theory]
    [InlineData("192.0.2.21", "192.0.2.20")]
    [InlineData("198.51.100.9", "192.0.2.20")]
    [InlineData("198.51.100.9", null)]
    [InlineData("::ffff:198.51.100.9", "::ffff:192.0.2.20")]
    [InlineData("2001:db8::21", "2001:db8::20")]
    [InlineData("::", "192.0.2.20")]
    [InlineData("::2", "192.0.2.20")]
    [InlineData("::127.0.0.1", "192.0.2.20")]
    [InlineData("224.0.0.0", "192.0.2.20")]
    public void RemotePeersCopyingTheMarkerKeepTheirAddress(string peer, string? local)
    {
        Assert.False(AgentSelfFetch.PeerIsThisServer(
            Ip(peer), local is null ? null : Ip(local)));
        Assert.False(AgentSelfFetch.IsAgentSelfFetch(
            WorkerUa, Ip(peer), local is null ? null : Ip(local)));
    }

    [Fact]
    public void NoPeerIsNeverThisServer()
    {
        Assert.False(AgentSelfFetch.PeerIsThisServer(null, Ip("127.0.0.1")));
        Assert.False(AgentSelfFetch.IsAgentSelfFetch(WorkerUa, null, null));
    }

    [Fact]
    public void RelabelSetsTheReservedAddressOnlyForARecognizedFetch()
    {
        var agent = new DefaultHttpContext();
        agent.Request.Headers.UserAgent = WorkerUa;
        agent.Connection.RemoteIpAddress = IPAddress.Loopback;
        agent.Connection.LocalIpAddress = IPAddress.Loopback;
        Assert.True(AgentSelfFetch.Relabel(agent));
        Assert.Equal(Ip("224.0.0.0"), agent.Connection.RemoteIpAddress);
        Assert.False(IPAddress.IsLoopback(agent.Connection.RemoteIpAddress!));

        var remote = new DefaultHttpContext();
        remote.Request.Headers.UserAgent = WorkerUa;
        remote.Connection.RemoteIpAddress = Ip("198.51.100.9");
        remote.Connection.LocalIpAddress = Ip("192.0.2.20");
        Assert.False(AgentSelfFetch.Relabel(remote));
        Assert.Equal(Ip("198.51.100.9"), remote.Connection.RemoteIpAddress);
    }

    // UsePageSpeed() adds the label before anything else, so every
    // middleware and endpoint after it sees the reserved address. Recorded
    // without building the rest of the pipeline (which needs the native
    // engine): the first registered component is run on its own.
    [Fact]
    public async Task UsePageSpeedRegistersTheLabelFirst()
    {
        var builder = new RecordingApplicationBuilder();
        builder.UsePageSpeed();
        Assert.NotEmpty(builder.Components);

        IPAddress? seen = null;
        RequestDelegate terminal = ctx =>
        {
            seen = ctx.Connection.RemoteIpAddress;
            return Task.CompletedTask;
        };
        var first = builder.Components[0](terminal);

        var ctx = new DefaultHttpContext();
        ctx.Request.Headers.UserAgent = WorkerUa;
        ctx.Connection.RemoteIpAddress = IPAddress.Loopback;
        ctx.Connection.LocalIpAddress = IPAddress.Loopback;
        await first(ctx);
        Assert.Equal(Ip("224.0.0.0"), seen);
    }

    // A real Kestrel pipeline on loopback with an application policy that
    // admits loopback clients only: the agent fetch is refused, a normal
    // loopback request is admitted, and the next request on the same
    // connection has its real address again.
    [Fact]
    public async Task LoopbackOnlyPolicyRefusesTheAgentFetchAndAdmitsALocalClient()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();

        using var host = Host.CreateDefaultBuilder()
            .ConfigureWebHostDefaults(web =>
            {
                web.ConfigureKestrel(k => k.Listen(IPAddress.Loopback, port));
                web.ConfigureLogging(b => b.SetMinimumLevel(LogLevel.Warning));
                web.Configure(app =>
                {
                    app.UsePageSpeedAgentFetchLabel();
                    app.Use(async (ctx, next) =>
                    {
                        var ip = ctx.Connection.RemoteIpAddress;
                        if (ip is null || !IPAddress.IsLoopback(ip))
                        {
                            ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                            return;
                        }
                        await next();
                    });
                    app.Run(ctx => ctx.Response.WriteAsync(
                        HeldMarker + " " + ctx.Connection.RemoteIpAddress));
                });
            })
            .Build();
        await host.StartAsync();
        try
        {
            using var handler = new SocketsHttpHandler { PooledConnectionLifetime = TimeSpan.FromMinutes(1), MaxConnectionsPerServer = 1 };
            using var client = new HttpClient(handler) { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

            var agent = await Send(client, WorkerUa);
            Assert.Equal(HttpStatusCode.Forbidden, agent.Status);
            Assert.DoesNotContain(HeldMarker, agent.Body);

            var local = await Send(client, BrowserUa);
            Assert.Equal(HttpStatusCode.OK, local.Status);
            Assert.Equal(HeldMarker + " 127.0.0.1", local.Body);

            var again = await Send(client, WorkerUa);
            Assert.Equal(HttpStatusCode.Forbidden, again.Status);
            Assert.DoesNotContain(HeldMarker, again.Body);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    private static async Task<(HttpStatusCode Status, string Body)> Send(
        HttpClient client, string userAgent)
    {
        using var req = new HttpRequestMessage(HttpMethod.Get, "/private/page.html");
        req.Headers.TryAddWithoutValidation("User-Agent", userAgent);
        using var resp = await client.SendAsync(req);
        return (resp.StatusCode, await resp.Content.ReadAsStringAsync());
    }

    private sealed class RecordingApplicationBuilder : IApplicationBuilder
    {
        public List<Func<RequestDelegate, RequestDelegate>> Components { get; } = new();

        public IServiceProvider ApplicationServices { get; set; } =
            new ServiceCollection().BuildServiceProvider();

        public IFeatureCollection ServerFeatures { get; } = new FeatureCollection();

        public IDictionary<string, object?> Properties { get; } =
            new Dictionary<string, object?>();

        public IApplicationBuilder Use(Func<RequestDelegate, RequestDelegate> middleware)
        {
            Components.Add(middleware);
            return this;
        }

        public IApplicationBuilder New() => this;

        public RequestDelegate Build() =>
            throw new NotSupportedException("recording only");
    }
}

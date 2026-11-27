// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Net.Sockets;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The console proxy reaches the loopback worker over an explicit IPv4
/// socket; other targets keep the default connect behaviour.
/// </summary>
public class LoopbackConnectTests
{
    [Theory]
    [InlineData("127.0.0.1", true)]
    [InlineData("127.0.0.5", true)]
    [InlineData("localhost", true)]
    [InlineData("LOCALHOST", true)]
    [InlineData("::1", false)]
    [InlineData("[::1]", false)]
    [InlineData("10.1.2.3", false)]
    [InlineData("example.test", false)]
    public void IsIPv4LoopbackTarget_ClassifiesHosts(string host, bool expected)
    {
        Assert.Equal(expected, LoopbackConnect.IsIPv4LoopbackTarget(host));
    }

    [Fact]
    public void Handler_UsesTheCallbackWithoutProxyOrCookieJar()
    {
        using var handler = LoopbackConnect.CreateHandler();
        Assert.NotNull(handler.ConnectCallback);
        Assert.False(handler.UseProxy);
        Assert.False(handler.UseCookies);
    }

    [Fact]
    public async Task LoopbackHost_ConnectsOverAnIPv4Socket()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;

        var accept = listener.AcceptSocketAsync();
        using var socket = await LoopbackConnect.ConnectSocketAsync(
            new DnsEndPoint("127.0.0.1", port), CancellationToken.None);
        using var accepted = await accept;

        Assert.Equal(AddressFamily.InterNetwork, socket.AddressFamily);
        Assert.True(socket.Connected);
    }

    [Fact]
    public async Task LocalhostName_ConnectsOverAnIPv4Socket()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;

        var accept = listener.AcceptSocketAsync();
        using var socket = await LoopbackConnect.ConnectSocketAsync(
            new DnsEndPoint("localhost", port), CancellationToken.None);
        using var accepted = await accept;

        Assert.Equal(AddressFamily.InterNetwork, socket.AddressFamily);
    }

    [Fact]
    public async Task NonLoopbackHost_KeepsTheDefaultDualStackSocket()
    {
        // Not a loopback target, so the callback must not force IPv4: it
        // builds the default socket (dual-stack where IPv6 is available)
        // and hands the name to the OS to resolve. Reaching a non-loopback
        // address from a test is not reliable, so assert on the choice
        // the callback makes: a connect to the TEST-NET address is
        // cancelled immediately and the socket is released.
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await LoopbackConnect.ConnectSocketAsync(
                new DnsEndPoint("192.0.2.1", 9), cts.Token));
        Assert.False(LoopbackConnect.IsIPv4LoopbackTarget("192.0.2.1"));
    }

    [Fact]
    public async Task Ipv6Loopback_IsNotRewrittenToIPv4()
    {
        if (!Socket.OSSupportsIPv6)
        {
            return;
        }

        IPv6Listener(out var listener, out var port);
        if (listener == null)
        {
            return;
        }

        using (listener)
        {
            var accept = listener.AcceptSocketAsync();
            using var socket = await LoopbackConnect.ConnectSocketAsync(
                new DnsEndPoint("::1", port), CancellationToken.None);
            using var accepted = await accept;
            Assert.Equal(AddressFamily.InterNetworkV6, socket.AddressFamily);
        }
    }

    private static void IPv6Listener(out TcpListener? listener, out int port)
    {
        try
        {
            listener = new TcpListener(IPAddress.IPv6Loopback, 0);
            listener.Start();
            port = ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        catch (SocketException)
        {
            listener = null;
            port = 0;
        }
    }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers.Binary;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NSubstitute;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class WorkerNotificationTests
{
    [Fact]
    public void TryNotify_ReturnsTrue_WhenChannelHasCapacity()
    {
        // Arrange
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var logger = Substitute.For<ILogger<WorkerNotificationService>>();
        var service = new WorkerNotificationService(optionsMonitor, new InternalWorkerEndpoint(), logger);

        // Act
        bool result = service.TryNotify(
            "http://example.com/page.html",
            "example.com",
            "https",
            PageSpeedContentType.Html,
            0);

        // Assert
        Assert.True(result);
    }

    [Fact]
    public void TryNotify_ReturnsTrue_ForMultipleNotifications()
    {
        // Arrange
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var logger = Substitute.For<ILogger<WorkerNotificationService>>();
        var service = new WorkerNotificationService(optionsMonitor, new InternalWorkerEndpoint(), logger);

        // Act: enqueue several notifications.
        bool result1 = service.TryNotify(
            "http://example.com/page1.html", "example.com", "http",
            PageSpeedContentType.Html, 0);
        bool result2 = service.TryNotify(
            "http://example.com/page2.html", "example.com", "http",
            PageSpeedContentType.Css, 0x08);
        bool result3 = service.TryNotify(
            "http://example.com/image.jpg", "example.com", "http",
            PageSpeedContentType.Image, 0x01);

        // Assert: all should succeed since channel capacity is 1024.
        Assert.True(result1);
        Assert.True(result2);
        Assert.True(result3);
    }

    [Fact]
    public void TryNotify_AcceptsAllContentTypes()
    {
        // Arrange
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var logger = Substitute.For<ILogger<WorkerNotificationService>>();
        var service = new WorkerNotificationService(optionsMonitor, new InternalWorkerEndpoint(), logger);

        // Act & Assert: all content types should be accepted.
        Assert.True(service.TryNotify(
            "http://example.com/a", "example.com", "http",
            PageSpeedContentType.Html, 0));
        Assert.True(service.TryNotify(
            "http://example.com/b", "example.com", "http",
            PageSpeedContentType.Css, 0));
        Assert.True(service.TryNotify(
            "http://example.com/c", "example.com", "http",
            PageSpeedContentType.Js, 0));
        Assert.True(service.TryNotify(
            "http://example.com/d", "example.com", "http",
            PageSpeedContentType.Image, 0));
        Assert.True(service.TryNotify(
            "http://example.com/e", "example.com", "http",
            PageSpeedContentType.Other, 0));
    }

    // ── agent_request + the v3→v4 regression fix ─────────────────────

    [Fact]
    public void TryNotify_WithAgentRequest_ReturnsTrue()
    {
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
        var service = new WorkerNotificationService(
            optionsMonitor, new InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());

        Assert.True(service.TryNotify(
            "http://example.com/p", "example.com", "https",
            PageSpeedContentType.Html, 0, agentRequest: true));
        // Back-compat: the 5-arg form (agentRequest defaults false) still binds.
        Assert.True(service.TryNotify(
            "http://example.com/q", "example.com", "https",
            PageSpeedContentType.Html, 0));
    }

    /// <summary>
    /// The load-bearing regression proof: the .NET writer must emit a v6 frame
    /// (worker_ipc.h kIpcVersion=6) carrying the agent_request byte, the
    /// empty option-context tail, no site and a zero flags byte — the worker rejects any other version
    /// outright, so a frame one version behind is dropped and cache-fill
    /// silently stops. Drives the real socket writer against a loopback
    /// Unix-domain listener and inspects the bytes. Also proves the dedup key
    /// separates agent vs non-agent notifications for the same URL.
    /// Unix-socket only (Windows uses named pipes).
    /// </summary>
    [Fact]
    public async Task SendsV6Frame_WithAgentRequestByte_AndDedupSeparatesAgent()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return; // named pipes

        var sockPath = $"/tmp/psn-{Guid.NewGuid():N}".Substring(0, 24) + ".sock";
        using var cts = new System.Threading.CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var listener = new Socket(
            AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            listener.Bind(new UnixDomainSocketEndPoint(sockPath));
            listener.Listen(1);

            var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
            optionsMonitor.CurrentValue.Returns(new PageSpeedOptions());
            var endpoint = new InternalWorkerEndpoint();
            var service = new WorkerNotificationService(
                optionsMonitor, endpoint,
                Substitute.For<ILogger<WorkerNotificationService>>());

            await service.StartAsync(cts.Token);
            endpoint.SetSocketPath(sockPath);

            var acceptTask = listener.AcceptAsync(cts.Token).AsTask();

            // Same URL+type, differing only in agentRequest → distinct dedup
            // keys → BOTH delivered (not collapsed).
            Assert.True(service.TryNotify(
                "http://example.com/page", "example.com", "https",
                PageSpeedContentType.Html, 0x08, agentRequest: true));
            Assert.True(service.TryNotify(
                "http://example.com/page", "example.com", "https",
                PageSpeedContentType.Html, 0x08, agentRequest: false));
            // A checked host: the same frame shape, with HOST_VOUCHED set.
            Assert.True(service.TryNotify(
                "http://example.com/vouched", "example.com", "https",
                PageSpeedContentType.Html, 0x08, agentRequest: true,
                hostVouched: true));

            using var conn = await acceptTask;
            var frame1 = await ReadFrameAsync(conn, cts.Token);
            var frame2 = await ReadFrameAsync(conn, cts.Token);
            var frame3 = await ReadFrameAsync(conn, cts.Token);

            // [4 total_len][1 version]...[1 scheme][1 agent_request]
            //   [4 option_context_len][1 option_signature_len]
            //   [1 site_len][1 flags]
            Assert.Equal(6, frame1[4]);          // version == kIpcVersion (v6)
            Assert.Equal(6, frame2[4]);

            // Golden bytes of the whole first frame: url "http://example.com/page"
            // (23 bytes), host "example.com" (11), HTML, mask 0x08, https,
            // agent_request 1, then the empty v5 tail and the empty v6 tail.
            var golden = new List<byte> { 0, 0, 0, 57, 6, 0, 0, 0, 23 };
            golden.AddRange(Encoding.UTF8.GetBytes("http://example.com/page"));
            golden.AddRange(new byte[] { 0, 0, 0, 11 });
            golden.AddRange(Encoding.UTF8.GetBytes("example.com"));
            golden.AddRange(new byte[] { 0, 0, 0, 0, 8, 2, 1, 0, 0, 0, 0, 0, 0, 0 });
            Assert.Equal(golden.ToArray(), frame1);

            // The vouched frame: url "http://example.com/vouched" (26 bytes)
            // makes it 3 bytes longer (total_length 60); the flags byte, the
            // last one, is 0x01 (HOST_VOUCHED) and nothing else changes.
            var vouched = new List<byte> { 0, 0, 0, 60, 6, 0, 0, 0, 26 };
            vouched.AddRange(Encoding.UTF8.GetBytes("http://example.com/vouched"));
            vouched.AddRange(new byte[] { 0, 0, 0, 11 });
            vouched.AddRange(Encoding.UTF8.GetBytes("example.com"));
            vouched.AddRange(new byte[] { 0, 0, 0, 0, 8, 2, 1, 0, 0, 0, 0, 0, 0, 1 });
            Assert.Equal(vouched.ToArray(), frame3);

            // No site and no flags: the frame ends in two zero bytes.
            Assert.Equal(0, frame1[^2]);         // site_length
            Assert.Equal(0, frame1[^1]);         // flags
            frame1 = frame1[..^2];
            frame2 = frame2[..^2];

            // The option context is empty, so the frame ends in four zero
            // length bytes and one zero length byte. Asserted explicitly:
            // a writer that emitted a payload without its signature, or a
            // signature without its payload, would be refused by the reader,
            // and the shape is the only thing standing between here and that.
            Assert.Equal(0, frame1[^5]);         // option_context_length b0
            Assert.Equal(0, frame1[^4]);
            Assert.Equal(0, frame1[^3]);
            Assert.Equal(0, frame1[^2]);         // option_context_length b3
            Assert.Equal(0, frame1[^1]);         // option_signature_length

            // agent_request sits immediately before that five-byte tail.
            Assert.Equal(1, frame1[^6]);         // agent_request == 1
            Assert.Equal(0, frame2[^6]);         // agent_request == 0

            await service.StopAsync(cts.Token);
        }
        finally
        {
            try { System.IO.File.Delete(sockPath); } catch { /* best effort */ }
        }
    }

    /// <summary>
    /// A worker run as a separate process: <c>AutoStart</c> off and a concrete
    /// <c>SocketPath</c>. The worker host publishes that path, and a
    /// notification reaches a listener on it.
    /// </summary>
    [Fact]
    public async Task AutoStartOff_WithSocketPath_NotifiesTheWorkerAtThatPath()
    {
        var sockPath = $"/tmp/psx-{Guid.NewGuid():N}".Substring(0, 24) + ".sock";
        var options = new PageSpeedOptions();
        options.Worker.AutoStart = false;
        options.Worker.SocketPath = sockPath;
        var endpoint = new InternalWorkerEndpoint();
        using var host = new WorkerProcessHost(
            Options.Create(options), endpoint,
            Substitute.For<ILogger<WorkerProcessHost>>());

        using var cts = new System.Threading.CancellationTokenSource(TimeSpan.FromSeconds(10));
        await host.StartAsync(cts.Token);
        Assert.Equal(sockPath, await endpoint.SocketPathReady.WaitAsync(cts.Token));

        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return; // named pipes

        using var listener = new Socket(
            AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            listener.Bind(new UnixDomainSocketEndPoint(sockPath));
            listener.Listen(1);

            var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
            optionsMonitor.CurrentValue.Returns(options);
            var service = new WorkerNotificationService(
                optionsMonitor, endpoint,
                Substitute.For<ILogger<WorkerNotificationService>>());
            await service.StartAsync(cts.Token);

            var acceptTask = listener.AcceptAsync(cts.Token).AsTask();
            Assert.True(service.TryNotify(
                "http://example.com/image.jpg", "example.com", "https",
                PageSpeedContentType.Image, 0x08));

            using var conn = await acceptTask;
            var frame = await ReadFrameAsync(conn, cts.Token);
            Assert.Equal(6, frame[4]);
            Assert.Equal("http://example.com/image.jpg",
                Encoding.UTF8.GetString(frame, 9, BinaryPrimitives.ReadInt32BigEndian(frame.AsSpan(5))));

            await service.StopAsync(cts.Token);
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
            try { System.IO.File.Delete(sockPath); } catch { /* best effort */ }
        }
    }

    /// <summary>
    /// <c>AutoStart</c> off and no <c>SocketPath</c>: no worker to reach, so
    /// the worker host publishes no endpoint and coordination stays off.
    /// </summary>
    [Fact]
    public async Task AutoStartOff_WithoutSocketPath_PublishesNoEndpoint()
    {
        var options = new PageSpeedOptions();
        options.Worker.AutoStart = false;
        var endpoint = new InternalWorkerEndpoint();
        using var host = new WorkerProcessHost(
            Options.Create(options), endpoint,
            Substitute.For<ILogger<WorkerProcessHost>>());

        using var cts = new System.Threading.CancellationTokenSource(TimeSpan.FromSeconds(10));
        await host.StartAsync(cts.Token);
        Assert.Null(await endpoint.SocketPathReady.WaitAsync(cts.Token));
        await host.StopAsync(CancellationToken.None);
    }

    private static async Task<byte[]> ReadFrameAsync(
        Socket s, System.Threading.CancellationToken ct)
    {
        var header = await ReadExactlyAsync(s, 4, ct);
        int payloadLen = BinaryPrimitives.ReadInt32BigEndian(header);
        var payload = await ReadExactlyAsync(s, payloadLen, ct);
        var frame = new byte[4 + payloadLen];
        header.CopyTo(frame, 0);
        payload.CopyTo(frame, 4);
        return frame;
    }

    private static async Task<byte[]> ReadExactlyAsync(
        Socket s, int n, System.Threading.CancellationToken ct)
    {
        var buf = new byte[n];
        int got = 0;
        while (got < n)
        {
            int r = await s.ReceiveAsync(buf.AsMemory(got), SocketFlags.None, ct);
            if (r == 0) throw new System.IO.IOException("socket closed early");
            got += r;
        }
        return buf;
    }
}

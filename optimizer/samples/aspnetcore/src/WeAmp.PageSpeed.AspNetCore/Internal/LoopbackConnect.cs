// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Net.Http;
using System.Net.Sockets;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Connect callback for the console proxy's connections to the local worker.
/// The worker listens on the IPv4 loopback address only. The default
/// dual-stack socket is refused for that address by host-network containers
/// on some container runtimes, so a loopback target is reached over an
/// explicit IPv4 socket. Any other target, and IPv6 literals, connect the
/// way <see cref="SocketsHttpHandler"/> does by default.
/// </summary>
internal static class LoopbackConnect
{
    /// <summary>
    /// True when <paramref name="host"/> should be reached over IPv4
    /// loopback: <c>localhost</c> or an IPv4 loopback literal. IPv6 literals
    /// (including <c>::1</c>) are not rewritten.
    /// </summary>
    internal static bool IsIPv4LoopbackTarget(string host)
    {
        if (string.Equals(host, "localhost", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return IPAddress.TryParse(host, out var ip)
            && ip.AddressFamily == AddressFamily.InterNetwork
            && IPAddress.IsLoopback(ip);
    }

    /// <summary>Builds the socket and connects it; used by the callback.</summary>
    internal static async ValueTask<Socket> ConnectSocketAsync(
        DnsEndPoint endPoint, CancellationToken cancellationToken)
    {
        Socket socket;
        EndPoint target;
        if (IsIPv4LoopbackTarget(endPoint.Host))
        {
            socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            target = new IPEndPoint(
                IPAddress.TryParse(endPoint.Host, out var ip) ? ip : IPAddress.Loopback,
                endPoint.Port);
        }
        else
        {
            socket = new Socket(SocketType.Stream, ProtocolType.Tcp);
            target = endPoint;
        }

        socket.NoDelay = true;
        try
        {
            await socket.ConnectAsync(target, cancellationToken).ConfigureAwait(false);
            return socket;
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    /// <summary>The callback to assign to <see cref="SocketsHttpHandler.ConnectCallback"/>.</summary>
    internal static async ValueTask<Stream> ConnectAsync(
        SocketsHttpConnectionContext context, CancellationToken cancellationToken)
    {
        var socket = await ConnectSocketAsync(context.DnsEndPoint, cancellationToken)
            .ConfigureAwait(false);
        return new NetworkStream(socket, ownsSocket: true);
    }

    /// <summary>A handler that connects with <see cref="ConnectAsync"/>.</summary>
    internal static SocketsHttpHandler CreateHandler() => new()
    {
        ConnectCallback = ConnectAsync,
        // The only target is the local worker: never through a proxy, and
        // no shared cookie jar (request cookies are forwarded as headers).
        UseProxy = false,
        UseCookies = false,
    };
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Net;
using System.Net.Sockets;
using Microsoft.AspNetCore.Http;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Recognizes the worker's agent fetches (agent_optimize render and
/// llms.txt synthesis) arriving from this server. The worker fetches the
/// site's own pages with curl pinned to the site's own address, so when it
/// shares the host with the application the fetch arrives from loopback or
/// from the address it connects to, and an application rule that admits
/// local clients would admit it, whoever asked for the page being rendered.
/// A recognized fetch (the worker's User-Agent marker AND a peer that is
/// this server) is presented to the rest of the pipeline as
/// <see cref="Label"/>, a reserved address no client connects from. A
/// remote client that copies the User-Agent and connects directly keeps its
/// own address.
/// Same rule as the nginx module's <c>src/nginx/agent_self_fetch.h</c>.
/// Limitations: a fetch leaving from another of the host's addresses than
/// the one it connects to, or reaching the application through a proxy on
/// another host, is not recognized and keeps its address.
/// Behind a proxy on the same host every request arrives from this host;
/// recognition there rests on the User-Agent alone, so place
/// <c>UseForwardedHeaders()</c> before <c>UsePageSpeed()</c>.
/// </summary>
internal static class AgentSelfFetch
{
    /// <summary>
    /// The token the worker's agent fetches carry in their User-Agent
    /// ("Mozilla/5.0 (compatible; mod_pagespeed agent_optimize)"), matched
    /// as a case-sensitive substring.
    /// </summary>
    internal const string UserAgentMarker = "mod_pagespeed agent_optimize";

    /// <summary>The address a recognized agent fetch is given: 224.0.0.0.</summary>
    internal static readonly IPAddress Label = new(new byte[] { 224, 0, 0, 0 });

    private static readonly byte[] IPv6LoopbackBytes = IPAddress.IPv6Loopback.GetAddressBytes();

    /// <summary>Whether <paramref name="userAgent"/> carries the marker.</summary>
    internal static bool IsAgentFetchUserAgent(string? userAgent) =>
        userAgent is not null
        && userAgent.Contains(UserAgentMarker, StringComparison.Ordinal);

    /// <summary>
    /// Whether <paramref name="peer"/> is this server: a loopback address
    /// (127.0.0.0/8, ::1, IPv4-mapped 127.0.0.0/8) or the address the
    /// connection arrived on (<paramref name="local"/>, by address only; an
    /// IPv4-mapped address equals its IPv4 form). A null peer is never this
    /// server; a null local leaves only the loopback test.
    /// </summary>
    internal static bool PeerIsThisServer(IPAddress? peer, IPAddress? local)
    {
        if (peer is null)
        {
            return false;
        }
        var p = Canonical(peer);
        if (IsLoopback(p))
        {
            return true;
        }
        if (local is null)
        {
            return false;
        }
        var l = Canonical(local);
        return p.AddressFamily == l.AddressFamily
            && p.GetAddressBytes().AsSpan().SequenceEqual(l.GetAddressBytes());
    }

    /// <summary>Both conditions, read from the request's connection.</summary>
    internal static bool IsAgentSelfFetch(
        string? userAgent, IPAddress? peer, IPAddress? local) =>
        IsAgentFetchUserAgent(userAgent) && PeerIsThisServer(peer, local);

    /// <summary>
    /// Presents a recognized agent fetch as <see cref="Label"/> to every
    /// later reader of <see cref="ConnectionInfo.RemoteIpAddress"/>. Returns
    /// whether it did.
    /// </summary>
    internal static bool Relabel(HttpContext context)
    {
        var connection = context.Connection;
        if (!IsAgentSelfFetch(
                context.Request.Headers.UserAgent.ToString(),
                connection.RemoteIpAddress,
                connection.LocalIpAddress))
        {
            return false;
        }
        connection.RemoteIpAddress = Label;
        return true;
    }

    private static IPAddress Canonical(IPAddress a) =>
        a.IsIPv4MappedToIPv6 ? a.MapToIPv4() : a;

    private static bool IsLoopback(IPAddress a) =>
        a.AddressFamily switch
        {
            AddressFamily.InterNetwork => a.GetAddressBytes()[0] == 127,
            AddressFamily.InterNetworkV6 =>
                a.GetAddressBytes().AsSpan().SequenceEqual(IPv6LoopbackBytes),
            _ => false,
        };
}

/// <summary>
/// Applies <see cref="AgentSelfFetch.Relabel"/> before the rest of the
/// pipeline. Registered first by <c>UsePageSpeed()</c>.
/// </summary>
internal sealed class AgentSelfFetchMiddleware
{
    private readonly RequestDelegate _next;

    public AgentSelfFetchMiddleware(RequestDelegate next)
    {
        _next = next;
    }

    public Task InvokeAsync(HttpContext context)
    {
        AgentSelfFetch.Relabel(context);
        return _next(context);
    }
}

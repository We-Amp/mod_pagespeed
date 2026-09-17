// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net.Http.Headers;
using Microsoft.AspNetCore.Http;
using Yarp.ReverseProxy.Forwarder;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// YARP <see cref="HttpTransformer"/> for the Inverse-mode outbound loopback
/// forward (public Kestrel front door → bundled nginx optimize-proxy). The base
/// transformer copies the request headers (and drops hop-by-hop headers like
/// Connection/Keep-Alive/TE) and sets the destination URI; this override then:
/// <list type="bullet">
///   <item>sets upstream <c>Host</c> = the ORIGINAL client Host (NOT 127.0.0.1) —
///   cache-key/URL correctness: a 127.0.0.1 Host partitions the pagespeed cache
///   and emits 127.0.0.1 asset URLs;</item>
///   <item>REMOVES all inbound <c>X-Forwarded-Proto</c> then SETS exactly ONE
///   validated value (http|https; anything else → https). ngx_pagespeed's
///   ps_apply_x_forwarded_proto reads the FIRST XFP header, so a single
///   authoritative value is mandatory — appending a client-supplied XFP would let
///   an attacker's <c>http</c> win and downgrade asset URLs (mixed-content /
///   cache-poison);</item>
///   <item>appends <c>X-Forwarded-For</c> with the transport peer;</item>
///   <item>sets <c>X-PageSpeed-Hop</c> = inbound + 1 (default 1) — the tamper-
///   evident cross-hop termination counter;</item>
///   <item>sets <c>X-PageSpeed-Internal</c> = the per-process nonce (defense-in-
///   depth only);</item>
///   <item>strips <c>Accept-Encoding</c> to identity so nginx receives parseable
///   HTML and can recompress assets;</item>
///   <item>does NOT inject <c>mod_pagespeed</c> into the forwarded User-Agent (or
///   nginx would treat the top-level HTML as a pagespeed sub-request and skip
///   optimizing it).</item>
/// </list>
/// </summary>
public sealed class InverseForwardTransformer : HttpTransformer
{
    /// <summary>The cross-hop termination counter header.</summary>
    public const string HopHeader = "X-PageSpeed-Hop";

    /// <summary>The defense-in-depth internal nonce header.</summary>
    public const string InternalHeader = "X-PageSpeed-Internal";

    /// <summary>The forwarded-proto header ngx_pagespeed honors (RespectXForwardedProto).</summary>
    public const string ForwardedProtoHeader = "X-Forwarded-Proto";

    /// <summary>The forwarded-for header.</summary>
    public const string ForwardedForHeader = "X-Forwarded-For";

    private readonly InternalSidecarEndpoint _endpoint;

    public InverseForwardTransformer(InternalSidecarEndpoint endpoint)
    {
        _endpoint = endpoint;
    }

    public override async ValueTask TransformRequestAsync(
        HttpContext httpContext,
        HttpRequestMessage proxyRequest,
        string destinationPrefix,
        CancellationToken cancellationToken)
    {
        // Capture the inbound hop value BEFORE the base copies headers (which it
        // would then drop X-PageSpeed-Hop? — no, it copies; but read from the
        // original request so we are independent of header-copy ordering).
        var inboundHop = ReadHop(httpContext.Request.Headers[HopHeader]);

        // Base: copy request headers + drop hop-by-hop (Connection/Keep-Alive/TE),
        // and set the destination URI from destinationPrefix + path + query.
        await base.TransformRequestAsync(httpContext, proxyRequest, destinationPrefix, cancellationToken);

        // Host = the original client host (override YARP's destination-authority
        // rewrite). RequestUtilities.MakeDestinationAddress keeps the path/query;
        // the Host header drives the pagespeed cache key + emitted asset URLs.
        var originalHost = httpContext.Request.Host.Value;
        if (!string.IsNullOrEmpty(originalHost))
        {
            proxyRequest.Headers.Host = originalHost;
        }

        // X-Forwarded-Proto: REMOVE all inbound, then SET exactly one validated
        // value. First-match-wins in the module makes the single value mandatory.
        proxyRequest.Headers.Remove(ForwardedProtoHeader);
        proxyRequest.Headers.TryAddWithoutValidation(ForwardedProtoHeader, NormalizeScheme(httpContext.Request.Scheme));

        // X-Forwarded-For: append the transport peer. (nginx re-appends 127.0.0.1
        // on the inner hop via $proxy_add_x_forwarded_for.)
        var remoteIp = httpContext.Connection.RemoteIpAddress?.ToString();
        if (!string.IsNullOrEmpty(remoteIp))
        {
            proxyRequest.Headers.TryAddWithoutValidation(ForwardedForHeader, remoteIp);
        }

        // X-PageSpeed-Hop: REMOVE inbound, then SET inbound+1 (default 1). The
        // tamper-evident cross-hop termination counter, independent of identity.
        proxyRequest.Headers.Remove(HopHeader);
        proxyRequest.Headers.TryAddWithoutValidation(
            HopHeader, (inboundHop + 1).ToString(System.Globalization.CultureInfo.InvariantCulture));

        // X-PageSpeed-Internal: the per-process nonce (base64url), defense-in-depth.
        proxyRequest.Headers.Remove(InternalHeader);
        proxyRequest.Headers.TryAddWithoutValidation(InternalHeader, EncodeNonce(_endpoint.InternalNonce));

        // Accept-Encoding → identity (clear it) so nginx receives parseable,
        // uncompressed bytes and the optimizer can parse/rewrite/recompress.
        proxyRequest.Headers.Remove("Accept-Encoding");
        if (proxyRequest.Content is not null)
        {
            proxyRequest.Content.Headers.ContentEncoding.Clear();
        }
    }

    /// <summary>
    /// Base64url-encodes the internal nonce for the header value (a stable,
    /// header-safe representation of the per-process nonce bytes).
    /// </summary>
    internal static string EncodeNonce(byte[] nonce) =>
        Convert.ToBase64String(nonce).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    /// <summary>
    /// Normalizes a request scheme to the single validated value ngx_pagespeed
    /// accepts: <c>http</c> or <c>https</c>; anything else falls to <c>https</c>
    /// (fail-safe: never downgrade to http on an unknown scheme).
    /// </summary>
    internal static string NormalizeScheme(string? scheme)
    {
        if (string.Equals(scheme, "http", StringComparison.OrdinalIgnoreCase)) return "http";
        return "https";
    }

    private static int ReadHop(Microsoft.Extensions.Primitives.StringValues value)
    {
        // Use the FIRST value (tamper-evident: a multi-valued hop header is
        // suspect, but reading the first is consistent with the module's
        // first-match semantics; the >=2 reject in the middleware is the backstop).
        var first = value.Count > 0 ? value[0] : null;
        return int.TryParse(first, System.Globalization.NumberStyles.Integer,
            System.Globalization.CultureInfo.InvariantCulture, out var n) && n > 0
            ? n
            : 0;
    }
}

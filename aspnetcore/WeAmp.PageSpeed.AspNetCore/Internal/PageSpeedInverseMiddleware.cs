// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Security.Cryptography;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Yarp.ReverseProxy.Forwarder;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The real request-path middleware for <see cref="SidecarMode.Inverse"/>
/// (replaces the no-op <c>UsePageSpeed()</c>). Kestrel is the PUBLIC front door;
/// this middleware streams optimizable requests over loopback to the bundled
/// nginx optimize-proxy, which proxy_pass-es back to the PRIVATE raw-origin
/// Kestrel endpoint where this same middleware bypasses itself (the core loop
/// break).
///
/// <para>SECURITY: the loop break is keyed on KERNEL/LISTENER-set, unforgeable
/// signals — the per-connection <see cref="IRawOriginMarker"/> feature (set by
/// the raw-origin listener, PRIMARY) and the
/// <c>Connection.LocalPort == RawOriginLoopbackPort</c> compare (redundant). The
/// <c>X-PageSpeed-Internal</c> nonce is defense-in-depth only and NEVER a sole
/// gate. A tamper-evident <c>X-PageSpeed-Hop</c> counter is read FIRST and hard-
/// rejects (500) at depth &gt;= 2, independent of every identity check.</para>
/// </summary>
internal sealed class PageSpeedInverseMiddleware
{
    private readonly RequestDelegate _next;
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly ISidecarManager _sidecar;
    private readonly InternalSidecarEndpoint _endpoint;
    private readonly IPageSpeedForwarder _forwarder;
    private readonly ILogger<PageSpeedInverseMiddleware> _logger;

    // The .pagespeed. URL contract regex — byte-identical to the location in
    // NginxConfigGenerator (the {10} hash-segment length is the URL contract).
    private static readonly Regex PagespeedResourceRe =
        new(@"\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+", RegexOptions.Compiled);

    // Collapses runs of '/' so classification matches nginx's merge_slashes view.
    private static readonly Regex MultiSlashRe = new("/{2,}", RegexOptions.Compiled);

    // The five admin/diagnostic handler paths (lockstep with NginxConfigGenerator).
    private static readonly string[] AdminPaths =
    {
        "/pagespeed_admin",
        "/pagespeed_statistics",
        "/pagespeed_global_statistics",
        "/pagespeed_message",
        "/pagespeed_console",
    };

    private const string PagespeedStaticPrefix = "/pagespeed_static/";
    private const string BeaconPath = "/ngx_pagespeed_beacon";

    // global_constants.h:36 kModPagespeedSubrequestUserAgent — defense-in-depth.
    private const string ModPagespeedUserAgent = "mod_pagespeed";

    public PageSpeedInverseMiddleware(
        RequestDelegate next,
        IOptions<PageSpeedOptions> options,
        ISidecarManager sidecar,
        InternalSidecarEndpoint endpoint,
        IPageSpeedForwarder forwarder,
        ILogger<PageSpeedInverseMiddleware> logger)
    {
        _next = next;
        _options = options;
        _sidecar = sidecar;
        _endpoint = endpoint;
        _forwarder = forwarder;
        _logger = logger;
    }

    public async Task InvokeAsync(HttpContext context)
    {
        var opts = _options.Value;
        var peer = context.Connection.RemoteIpAddress;
        var peerIsLoopback = peer is not null && System.Net.IPAddress.IsLoopback(peer);

        // (0) HOP HYGIENE — a client-supplied X-PageSpeed-Hop is untrusted. Strip it
        // when the transport peer is NON-loopback (a real external client) so a
        // client can neither force the loop-ceiling 500 below nor skip optimization
        // at will. A loop-continuation always re-enters from the loopback nginx, so
        // on a loopback peer we KEEP the inbound hop — that is what lets the ceiling
        // bound a (structurally near-impossible) marker+port double-failure instead
        // of spinning forever.
        if (!peerIsLoopback)
        {
            context.Request.Headers.Remove(InverseForwardTransformer.HopHeader);
        }

        // (1) HOP CEILING — read FIRST, before any identity/classify decision. A
        // tamper-evident cross-hop termination backstop that terminates even if
        // every identity check fails. NOT stripped on the raw-origin branch.
        if (ReadHop(context) >= 2)
        {
            _logger.LogError(
                "PageSpeed Inverse: X-PageSpeed-Hop depth >= 2 on {Path}; hard-rejecting (loop backstop).",
                context.Request.Path);
            context.Response.StatusCode = StatusCodes.Status500InternalServerError;
            return;
        }

        // (2) LOOP BREAK — the core. If this request arrived on the PRIVATE raw-
        // origin (marker feature OR RawOriginLoopbackPort), serve the RAW origin
        // (bypass ourselves). Both signals are kernel/listener-set, unforgeable.
        if (IsRawOriginRequest(context))
        {
            // Defense-in-depth: assert the TRANSPORT peer is loopback (read above
            // from Connection.RemoteIpAddress, BEFORE any UseForwardedHeaders rewrite,
            // so it is the un-rewritten transport peer). A non-loopback peer on the
            // raw-origin branch is a misconfiguration or an attack → 421 + abort
            // (HARD reject, never silent bypass).
            if (!peerIsLoopback)
            {
                _logger.LogError(
                    "PageSpeed Inverse: raw-origin request from non-loopback transport peer {Peer}; rejecting 421.",
                    peer);
                context.Response.StatusCode = StatusCodes.Status421MisdirectedRequest;
                context.Abort();
                return;
            }

            // Constant-time verify + strip the nonce (defense-in-depth only — the
            // bypass decision was already made by the marker/port, NOT the nonce).
            VerifyAndStripNonce(context);
            // X-PageSpeed-Hop is intentionally NOT stripped here (read-first backstop).

            await _next(context);
            return;
        }

        // We are on the PUBLIC port. Strip any inbound nonce (untrusted here) — a
        // spoofed nonce on the public port must NEVER cause a bypass.
        context.Request.Headers.Remove(InverseForwardTransformer.InternalHeader);

        // (3) PageSpeed disabled → pass through.
        if (!opts.Enabled)
        {
            await _next(context);
            return;
        }

        // (4) Sidecar not running → graceful degradation (serve un-optimized,
        // the design record always-functional) rather than 502.
        if (_sidecar.State != SidecarState.Running)
        {
            await _next(context);
            return;
        }

        // (5) WebSocket upgrade → pass through (the loopback hop cannot carry it).
        if (context.WebSockets.IsWebSocketRequest)
        {
            await _next(context);
            return;
        }

        // (6) mod_pagespeed UA (independent defense-in-depth) → pass through. NEVER
        // the sole forward-vs-bypass decision; the marker/port already decided this
        // is the public port, so a UA match here just declines to forward.
        if (HasModPagespeedUserAgent(context))
        {
            await _next(context);
            return;
        }

        // (6b) HOST HANDLING — forward-all by default (zero config). mod_pagespeed
        // IMPLICITLY authorizes a request's OWN same-origin resources with NO
        // `pagespeed Domain` directive (DomainLawyer::IsDomainAuthorized: same Origin
        // => authorized), so every host the app serves is optimizable out of the box;
        // the `Domain` list governs only CROSS-origin (CDN) rewriting, which stays
        // un-widened. A Host-less request (HTTP/1.0 / blank Host) gives the module
        // nothing to key on, so bypass it un-optimized.
        if (string.IsNullOrEmpty(context.Request.Host.Host))
        {
            await _next(context);
            return;
        }

        // Optional defense-in-depth: when the operator opts into
        // RestrictToAuthorizedHosts, forward ONLY hosts in Domains.AuthorizedDomains
        // (localhost/127.0.0.1 are always allowed). OFF by default — the Host-keyed
        // cache-poisoning/reflection surface is bounded (size-capped, Host-fragmented
        // caches with eviction) and is the SAME exposure as running nginx+pagespeed in
        // front of the app, so forward-all adds nothing the product hasn't shipped.
        if (opts.Sidecar.RestrictToAuthorizedHosts && !HostIsAuthorized(context.Request.Host, opts))
        {
            await _next(context);
            return;
        }

        // Classify on the slash-collapsed path: nginx (merge_slashes on by default)
        // normalizes repeated slashes before routing, so the middleware MUST classify
        // on the same normalized view or e.g. '//pagespeed_admin' slips the admin gate.
        var path = NormalizePath(context.Request.Path.Value);

        // (7) Admin paths from the PUBLIC endpoint: 404 by default. The module's
        // real-TCP-peer IP-ACL is nullified in Inverse (nginx always sees the
        // loopback middleware), so forwarding admin would expose it. Forward ONLY
        // when AllowPublicAdmin AND AdminAuth.Enabled (bearer is load-bearing; the
        // validator hard-rejects Inverse + AdminAuth disabled).
        if (IsAdminPath(path))
        {
            if (opts.Sidecar.AllowPublicAdmin && opts.AdminAuth.Enabled)
            {
                await ForwardAsync(context);
                return;
            }
            context.Response.StatusCode = StatusCodes.Status404NotFound;
            return;
        }

        var isManaged = IsPageSpeedManagedPath(path);

        // (8) Engine-owned managed paths (.pagespeed. resource URLs, /pagespeed_static/,
        // the instrumentation beacon) MUST reach nginx for EVERY method — nginx, not
        // Kestrel, owns that URL space, so bypassing them to the app would 404. The
        // beacon may arrive as GET or POST (it switches to POST only when the payload
        // exceeds the GET-URL length), so gate on the managed path, not the method. (The
        // raw-origin loop-break already returned at step (2) far upstream, so a forwarded
        // request re-entering on the private endpoint never reaches here.)
        if (isManaged)
        {
            await ForwardAsync(context);
            return;
        }

        // (9) Non-managed, non-GET/HEAD → pass straight through to the app.
        // mod_pagespeed optimizes and caches ONLY cacheable GET/HEAD responses;
        // POST/PUT/DELETE/PATCH/OPTIONS/CONNECT/… are never rewritten or cached, so the
        // loopback hop to nginx would buy nothing and the direct bypass is one hop
        // cheaper (same behavior as 2.0). HEAD is optimized like GET; Range and
        // conditional (If-*) requests are GETs and are forwarded below.
        var isGetOrHead = HttpMethods.IsGet(context.Request.Method) || HttpMethods.IsHead(context.Request.Method);
        if (!isGetOrHead)
        {
            await _next(context);
            return;
        }

        // (10) ExcludePaths (reuse 2.0 IsExcluded/GlobMatch) → pass through. Managed
        // paths already forwarded at (8), so they never reach this gate.
        if (IsExcluded(context.Request, opts))
        {
            await _next(context);
            return;
        }

        // (11) Optimizable GET/HEAD → FORWARD to nginx (streaming).
        await ForwardAsync(context);
    }

    // ----------------------------------------------------------------------
    // Loop break + classification helpers (exposed internal for unit tests).
    // ----------------------------------------------------------------------

    /// <summary>
    /// True when the request arrived on the PRIVATE raw-origin endpoint: the
    /// per-connection <see cref="IRawOriginMarker"/> feature is present (PRIMARY,
    /// listener-set) OR <c>Connection.LocalPort == RawOriginLoopbackPort</c>
    /// (redundant kernel signal). Either alone is sufficient and unforgeable.
    /// </summary>
    internal bool IsRawOriginRequest(HttpContext context)
    {
        if (context.Features.Get<IRawOriginMarker>() is not null)
        {
            return true;
        }
        return _endpoint.IsRawOriginLocalPort(context.Connection.LocalPort);
    }

    /// <summary>
    /// True for an engine-owned path nginx must terminate: a .pagespeed. resource
    /// URL, the /pagespeed_static/ prefix, or the beacon. These are forwarded (NOT
    /// bypassed: Kestrel 404s them) and are EXEMPT from ExcludePaths.
    /// </summary>
    internal static bool IsPageSpeedManagedPath(string path)
    {
        if (string.IsNullOrEmpty(path)) return false;
        if (path.StartsWith(PagespeedStaticPrefix, StringComparison.Ordinal)) return true;
        if (IsBeaconPath(path)) return true;
        return PagespeedResourceRe.IsMatch(path);
    }

    private static bool IsBeaconPath(string path) =>
        string.Equals(path, BeaconPath, StringComparison.Ordinal);

    /// <summary>
    /// Collapses runs of '/' to a single '/' so middleware classification matches
    /// what nginx (merge_slashes on by default) routes on. Kestrel pre-resolves
    /// dot-segments, so slash-folding is the remaining normalization gap (e.g.
    /// "//pagespeed_admin" must classify as the admin path, not slip the gate).
    /// </summary>
    internal static string NormalizePath(string? path)
    {
        if (string.IsNullOrEmpty(path)) return string.Empty;
        return path.Contains("//", StringComparison.Ordinal)
            ? MultiSlashRe.Replace(path, "/")
            : path;
    }

    /// <summary>
    /// True when the request Host is allowed under the OPT-IN strict allowlist
    /// (<see cref="SidecarOptions.RestrictToAuthorizedHosts"/>): loopback
    /// (localhost/127.0.0.1/::1) is ALWAYS allowed, otherwise the Host must be in
    /// <see cref="DomainOptions.AuthorizedDomains"/> (exact case-insensitive, or
    /// wildcard via <see cref="GlobMatch"/>). NOT consulted on the default
    /// forward-all path — only when an operator turns restriction on.
    /// </summary>
    internal static bool HostIsAuthorized(HostString host, PageSpeedOptions opts)
    {
        var h = host.Host; // HostString.Host is the host WITHOUT the port
        if (string.IsNullOrEmpty(h)) return false;
        // Loopback is ALWAYS authorized: local dev, the sidecar health probe, and the
        // raw-origin loopback leg can never be locked out by a strict allowlist.
        if (string.Equals(h, "localhost", StringComparison.OrdinalIgnoreCase)
            || h == "127.0.0.1" || h == "::1" || h == "[::1]")
        {
            return true;
        }
        var domains = opts.Domains?.AuthorizedDomains;
        if (domains is null) return false;
        foreach (var d in domains)
        {
            if (string.IsNullOrEmpty(d)) continue;
            var pat = StripToHost(d); // tolerate scheme/port in the configured entry
            if (pat.Length == 0) continue;
            if (pat.AsSpan().IndexOfAny('*', '?') >= 0)
            {
                if (GlobMatch(h, pat)) return true;
            }
            else if (string.Equals(h, pat, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    // Reduces an AuthorizedDomains entry to its host: strips a scheme prefix
    // (http://host), a path (host/x), and a :port (host:443).
    private static string StripToHost(string domain)
    {
        var s = domain.Trim();
        var scheme = s.IndexOf("://", StringComparison.Ordinal);
        if (scheme >= 0) s = s[(scheme + 3)..];
        var slash = s.IndexOf('/');
        if (slash >= 0) s = s[..slash];
        var colon = s.IndexOf(':');
        if (colon >= 0) s = s[..colon];
        return s;
    }

    private static bool IsAdminPath(string path)
    {
        foreach (var admin in AdminPaths)
        {
            // Case-insensitive prefix (the module dispatches admin case-insensitively).
            if (path.StartsWith(admin, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    private static bool HasModPagespeedUserAgent(HttpContext context)
    {
        var ua = context.Request.Headers.UserAgent.ToString();
        return ua.Contains(ModPagespeedUserAgent, StringComparison.OrdinalIgnoreCase);
    }

    private static int ReadHop(HttpContext context)
    {
        var v = context.Request.Headers[InverseForwardTransformer.HopHeader];
        var first = v.Count > 0 ? v[0] : null;
        return int.TryParse(first, System.Globalization.NumberStyles.Integer,
            System.Globalization.CultureInfo.InvariantCulture, out var n) && n > 0
            ? n
            : 0;
    }

    private void VerifyAndStripNonce(HttpContext context)
    {
        var header = context.Request.Headers[InverseForwardTransformer.InternalHeader];
        var presented = header.Count > 0 ? header[0] : null;
        context.Request.Headers.Remove(InverseForwardTransformer.InternalHeader);

        if (presented is null)
        {
            // No nonce on a marker/port-identified raw-origin request: tolerated
            // (the marker/port is authoritative). Log at debug only.
            _logger.LogDebug("PageSpeed Inverse: raw-origin request carried no internal nonce (marker/port authoritative).");
            return;
        }

        var expected = InverseForwardTransformer.EncodeNonce(_endpoint.InternalNonce);
        var match = CryptographicOperations.FixedTimeEquals(
            System.Text.Encoding.ASCII.GetBytes(presented),
            System.Text.Encoding.ASCII.GetBytes(expected));
        if (!match)
        {
            // A mismatch on a marker-identified connection is suspicious but NOT a
            // bypass gate (the marker already proved raw-origin). Log a warning.
            _logger.LogWarning("PageSpeed Inverse: raw-origin request carried a non-matching internal nonce (ignored; marker/port authoritative).");
        }
    }

    /// <summary>
    /// Checks if the request path matches any exclusion pattern. Patterns with
    /// * or ? are matched as globs; others as prefixes. Reused verbatim from the
    /// 2.0 PageSpeedMiddleware (IsExcluded/GlobMatch).
    /// </summary>
    internal static bool IsExcluded(HttpRequest request, PageSpeedOptions opts)
    {
        var path = request.Path.Value;
        if (path == null) return false;

        foreach (var pattern in opts.ExcludePaths)
        {
            if (pattern.AsSpan().IndexOfAny('*', '?') >= 0)
            {
                if (GlobMatch(path, pattern)) return true;
            }
            else if (path.StartsWith(pattern, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>
    /// Simple glob matcher supporting * (any sequence) and ? (single char),
    /// case-insensitive. Reused verbatim from the 2.0 PageSpeedMiddleware.
    /// </summary>
    internal static bool GlobMatch(ReadOnlySpan<char> input, ReadOnlySpan<char> pattern)
    {
        int ip = 0, pp = 0;
        int starIp = -1, starPp = -1;

        while (ip < input.Length)
        {
            if (pp < pattern.Length &&
                (char.ToLowerInvariant(pattern[pp]) == char.ToLowerInvariant(input[ip]) ||
                 pattern[pp] == '?'))
            {
                ip++;
                pp++;
            }
            else if (pp < pattern.Length && pattern[pp] == '*')
            {
                starPp = pp++;
                starIp = ip;
            }
            else if (starPp >= 0)
            {
                pp = starPp + 1;
                ip = ++starIp;
            }
            else
            {
                return false;
            }
        }

        while (pp < pattern.Length && pattern[pp] == '*') pp++;
        return pp == pattern.Length;
    }

    private async Task ForwardAsync(HttpContext context)
    {
        var nginxPort = _endpoint.NginxLoopbackPort;
        if (nginxPort == 0)
        {
            // Should never happen once the raw-origin configurator ran; fail open.
            _logger.LogWarning("PageSpeed Inverse: nginx loopback port not set; serving un-optimized (fail open).");
            await _next(context);
            return;
        }

        var destinationPrefix = string.Create(
            System.Globalization.CultureInfo.InvariantCulture, $"http://127.0.0.1:{nginxPort}");

        var error = await _forwarder.SendAsync(context, destinationPrefix, context.RequestAborted);

        if (error != ForwarderError.None)
        {
            if (!context.Response.HasStarted)
            {
                // Graceful degradation: nothing written yet → fall through to the
                // origin (serve un-optimized) rather than surface a 502. Reset any
                // status/headers YARP set from the failed forward (it sets e.g.
                // 502/504 before returning the error) so the origin response starts
                // clean (a downstream endpoint that writes a body without setting a
                // status would otherwise emit it under the stale 5xx).
                _logger.LogWarning(
                    "PageSpeed Inverse: forward to nginx failed ({Error}) before response start; serving un-optimized (fail open).",
                    error);
                context.Response.Clear();
                await _next(context);
            }
            else
            {
                // Response already started → cannot fall back; abort + log.
                _logger.LogError(
                    "PageSpeed Inverse: forward to nginx failed ({Error}) after response start; aborting.", error);
                context.Abort();
            }
        }
    }
}

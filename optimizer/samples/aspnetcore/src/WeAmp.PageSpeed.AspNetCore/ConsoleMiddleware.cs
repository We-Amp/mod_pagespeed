// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Collections.Concurrent;
using System.Net;
using System.Net.Http;
using System.Net.WebSockets;
using System.Reflection;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Serves the embedded /console/ SvelteKit SPA and reverse-proxies the
/// SPA's worker API — /v1/health, /v1/ws, /v1/cache,
/// /v1/stats, /v1/capture and /v1/config — to the loopback
/// factory_worker. Registered in front of <see cref="PageSpeedMiddleware"/>
/// by <c>UsePageSpeed()</c>.
/// </summary>
/// <remarks>
/// SPA assets live in the main assembly's manifest under the logical
/// prefix <c>WeAmp.PageSpeed.AspNetCore.Console/</c>. The prefix is
/// stripped to produce the asset's POSIX-style relative path, which is
/// what the request URL maps onto after the configurable mount path.
/// </remarks>
public sealed class ConsoleMiddleware
{
    /// <summary>
    /// Logical-name prefix every embedded SPA asset shares. Anything
    /// inside <c>tools/workbench/packages/web-shell/dist/</c> at pack
    /// time lands under this prefix. See the <c>WebShellEmbed</c>
    /// target in the csproj.
    /// </summary>
    internal const string ResourcePrefix = "WeAmp.PageSpeed.AspNetCore.Console/";

    // Prefixes that ConsoleMiddleware reverse-proxies to the loopback
    // worker. /v1/health is HTTP REST. /v1/ws is the
    // WebSocket family (/v1/ws/logs, /v1/ws/stats, /v1/ws/events) that
    // the SPA subscribes to for live streaming; those requests arrive
    // as HTTP/1.1 Upgrade and are bridged to the worker via a loopback
    // ClientWebSocket — see ProxyWebSocketAsync.
    //
    // The data endpoints back the SPA's Dashboard / Configuration / URLs
    // / Savings / Metrics pages, which the minimal v2.0.12 surface left
    // unproxied (they 404'd in-process; the Docker demo masked it by
    // reaching the worker directly):
    //   /v1/stats     — GET, aggregate counters (Dashboard / Metrics).
    //   /v1/cache     — GET list/select/content/cooldowns; POST purge /
    //                   reprocess. Segment-aware, so this also covers
    //                   /v1/cache/urls, /v1/cache/purge, etc.
    //   /v1/capture   — POST waterfall / screenshot capture jobs (Savings).
    //   /v1/config    — GET current config; PATCH config updates.
    // These are plain HTTP REST; ProxyToWorkerAsync forwards every method
    // (GET/POST/PUT/PATCH/DELETE) with its body and headers generically,
    // so no per-prefix or per-method logic is needed here. The mutating
    // calls carry X-Requested-With (set by the SPA transport); the
    // middleware forwards it untouched so the worker's authoritative CSRF
    // gate can see it — the middleware itself adds no CSRF check.
    private static readonly string[] ProxyPrefixes =
    {
        "/v1/health", "/v1/ws",
        "/v1/cache", "/v1/stats", "/v1/capture", "/v1/config",
    };

    // HTTP/1.1 hop-by-hop headers (RFC 7230 §6.1). These must not be
    // forwarded across a proxy hop; copying them confuses Kestrel and
    // can leak connection state.
    private static readonly HashSet<string> HopByHopHeaders =
        new(StringComparer.OrdinalIgnoreCase)
        {
            "Connection",
            "Keep-Alive",
            "Proxy-Authenticate",
            "Proxy-Authorization",
            "TE",
            "Trailer",
            "Transfer-Encoding",
            "Upgrade",
            "Host",
            "Content-Length",
            // Strip forwarding headers on this internal proxy hop —
            // we're going from Kestrel to factory_worker on 127.0.0.1.
            // The worker should never see the external client's IP /
            // host chain leaked from the customer's reverse-proxy
            // headers (which are attacker-controllable if the customer's
            // ingress isn't strict about who can set them).
            "X-Forwarded-For",
            "X-Forwarded-Host",
            "X-Forwarded-Proto",
            "X-Forwarded-Port",
            "X-Forwarded-Server",
            "X-Real-IP",
            "Forwarded",
        };

    // Connects the upstream WebSocket to the loopback worker over IPv4.
    // Shared and never disposed: it holds no per-connection state.
    private static readonly HttpMessageInvoker WorkerWebSocketInvoker =
        new(LoopbackConnect.CreateHandler(), disposeHandler: false);

    private readonly RequestDelegate _next;
    private readonly IOptionsMonitor<PageSpeedOptions> _options;
    private readonly InternalWorkerEndpoint _endpoint;
    private readonly IHttpClientFactory _httpClientFactory;
    private readonly ILogger<ConsoleMiddleware> _logger;
    private readonly Assembly _assembly;
    private readonly Lazy<HashSet<string>> _resourceNames;

    // Tiny per-process asset cache. The embedded payload is small
    // (~744 KB total, ~60 files) so we eagerly cache decompressed
    // bytes on first hit per file. Keyed by logical asset name
    // ("index.html", "_app/immutable/foo.js", …).
    private readonly ConcurrentDictionary<string, byte[]> _assetCache = new();

    /// <summary>Standard ASP.NET Core middleware constructor.</summary>
    public ConsoleMiddleware(
        RequestDelegate next,
        IOptionsMonitor<PageSpeedOptions> options,
        InternalWorkerEndpoint endpoint,
        IHttpClientFactory httpClientFactory,
        ILogger<ConsoleMiddleware> logger)
    {
        _next = next;
        _options = options;
        _endpoint = endpoint;
        _httpClientFactory = httpClientFactory;
        _logger = logger;
        _assembly = typeof(ConsoleMiddleware).Assembly;
        _resourceNames = new Lazy<HashSet<string>>(LoadResourceNames);
    }

    private HashSet<string> LoadResourceNames()
    {
        return new HashSet<string>(
            _assembly.GetManifestResourceNames()
                .Where(n => n.StartsWith(ResourcePrefix, StringComparison.Ordinal)),
            StringComparer.Ordinal);
    }

    /// <summary>Middleware pipeline entry point.</summary>
    public async Task InvokeAsync(HttpContext context)
    {
        var opts = _options.CurrentValue;
        var consoleOpts = opts.Console;

        if (!consoleOpts.Enabled)
        {
            await _next(context);
            return;
        }

        var path = context.Request.Path;
        var mountPath = consoleOpts.MountPath;
        // Reject degenerate mount-path values that would claim every
        // URL on the host. Pass through silently so the customer's app
        // stays reachable; log once so the misconfiguration is visible.
        bool mountPathValid = !string.IsNullOrEmpty(mountPath)
            && mountPath != "/"
            && mountPath.StartsWith('/');
        if (!mountPathValid)
        {
            _logger.LogWarning(
                "Console.MountPath {MountPath} is invalid (must be a non-root absolute path " +
                "like \"/console\"); console middleware passing through.",
                mountPath ?? "<null>");
            await _next(context);
            return;
        }
        bool isConsole = path.StartsWithSegments(mountPath, out _);
        bool isProxy = IsProxyPath(path);

        if (!isConsole && !isProxy)
        {
            await _next(context);
            return;
        }

        if (consoleOpts.RequireHttps && !context.Request.IsHttps)
        {
            context.Response.StatusCode = StatusCodes.Status403Forbidden;
            await context.Response.WriteAsync(
                "HTTPS required for the PageSpeed console.");
            return;
        }

        if (isConsole)
        {
            await ServeConsoleAsync(context, mountPath);
            return;
        }

        // isProxy: WebSocket upgrade requests go through the WS bridge;
        // everything else uses the HttpClient-based REST proxy.
        if (context.WebSockets.IsWebSocketRequest)
        {
            await ProxyWebSocketAsync(context);
            return;
        }
        await ProxyToWorkerAsync(context);
    }

    private static bool IsProxyPath(PathString path)
    {
        foreach (var prefix in ProxyPrefixes)
        {
            if (path.StartsWithSegments(prefix, out _))
                return true;
        }
        return false;
    }

    private async Task ServeConsoleAsync(HttpContext context, string mountPath)
    {
        // Strip the mount-path prefix to get the asset name. Use
        // StartsWithSegments to get the remaining path; an empty
        // remainder means the customer hit the mount root.
        context.Request.Path.StartsWithSegments(
            mountPath, out var remaining);
        var rel = remaining.HasValue ? remaining.Value!.TrimStart('/') : "";

        if (rel.Length == 0)
        {
            await ServeIndexHtmlAsync(context);
            return;
        }

        var resourceName = ResourcePrefix + rel;
        if (_resourceNames.Value.Contains(resourceName))
        {
            await ServeStaticAssetAsync(context, rel, resourceName);
            return;
        }

        // No literal asset hit. SPA fallback: deep-link requests like
        // /console/metrics should render index.html so the client-side
        // router can take over. Heuristic: no file extension, or the
        // client accepts HTML.
        if (LooksLikeSpaRoute(rel, context.Request))
        {
            await ServeIndexHtmlAsync(context);
            return;
        }

        context.Response.StatusCode = StatusCodes.Status404NotFound;
    }

    private static bool LooksLikeSpaRoute(string rel, HttpRequest request)
    {
        // If the last path segment has a "." we treat it as a file.
        var lastSlash = rel.LastIndexOf('/');
        var lastSegment = lastSlash < 0 ? rel : rel[(lastSlash + 1)..];
        bool hasExtension = lastSegment.Contains('.', StringComparison.Ordinal);
        if (!hasExtension) return true;

        var accept = request.Headers.Accept.ToString();
        return accept.Contains("text/html", StringComparison.OrdinalIgnoreCase);
    }

    private async Task ServeIndexHtmlAsync(HttpContext context)
    {
        var name = ResourcePrefix + "index.html";
        if (!_resourceNames.Value.Contains(name))
        {
            _logger.LogError(
                "Console SPA index.html missing from embedded resources " +
                "(expected logical name {Name}). Did the workbench build " +
                "run during the .NET build?", name);
            context.Response.StatusCode = StatusCodes.Status503ServiceUnavailable;
            await context.Response.WriteAsync(
                "PageSpeed console assets are missing from this build.");
            return;
        }

        var bytes = LoadAssetBytes(name);
        context.Response.StatusCode = StatusCodes.Status200OK;
        context.Response.ContentType = "text/html; charset=utf-8";
        context.Response.Headers.CacheControl = "no-cache";
        context.Response.ContentLength = bytes.Length;
        await context.Response.Body.WriteAsync(bytes);
    }

    private async Task ServeStaticAssetAsync(
        HttpContext context, string rel, string resourceName)
    {
        var bytes = LoadAssetBytes(resourceName);
        context.Response.StatusCode = StatusCodes.Status200OK;
        context.Response.ContentType = ContentTypeFor(rel);
        if (rel.Contains("_app/immutable/", StringComparison.Ordinal))
        {
            context.Response.Headers.CacheControl =
                "public, max-age=31536000, immutable";
        }
        else
        {
            context.Response.Headers.CacheControl = "no-cache";
        }
        context.Response.ContentLength = bytes.Length;
        await context.Response.Body.WriteAsync(bytes);
    }

    private byte[] LoadAssetBytes(string resourceName)
    {
        return _assetCache.GetOrAdd(resourceName, name =>
        {
            using var stream = _assembly.GetManifestResourceStream(name)
                ?? throw new InvalidOperationException(
                    $"Embedded resource {name} disappeared between manifest " +
                    "scan and read — assembly was likely swapped underneath us.");
            using var ms = new MemoryStream();
            stream.CopyTo(ms);
            return ms.ToArray();
        });
    }

    private static string ContentTypeFor(string rel)
    {
        var dot = rel.LastIndexOf('.');
        if (dot < 0) return "application/octet-stream";
        var ext = rel[(dot + 1)..].ToLowerInvariant();
        return ext switch
        {
            "html" => "text/html; charset=utf-8",
            "js" or "mjs" => "application/javascript; charset=utf-8",
            "css" => "text/css; charset=utf-8",
            "json" => "application/json; charset=utf-8",
            "map" => "application/json; charset=utf-8",
            "svg" => "image/svg+xml",
            "png" => "image/png",
            "jpg" or "jpeg" => "image/jpeg",
            "gif" => "image/gif",
            "webp" => "image/webp",
            "avif" => "image/avif",
            "ico" => "image/x-icon",
            "woff" => "font/woff",
            "woff2" => "font/woff2",
            "ttf" => "font/ttf",
            "otf" => "font/otf",
            "txt" => "text/plain; charset=utf-8",
            "wasm" => "application/wasm",
            _ => "application/octet-stream",
        };
    }

    // Bridge a browser-side WebSocket upgrade to a loopback WebSocket
    // connection on factory_worker. The middleware terminates the
    // browser-side handshake itself (via AcceptWebSocketAsync), opens a
    // ClientWebSocket against http://127.0.0.1:{workerPort}{path} (the
    // ws:// equivalent), and then pumps frames in both directions
    // until either side closes.
    //
    // The Sec-WebSocket-Protocol header is forwarded so the worker can
    // negotiate subprotocols (currently unused, but cheap to support).
    // All other hop-by-hop headers and the Sec-WebSocket-* dance are
    // managed by Kestrel + ClientWebSocket — we don't touch them.
    private async Task ProxyWebSocketAsync(HttpContext context)
    {
        int port;
        try
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(
                context.RequestAborted);
            cts.CancelAfter(TimeSpan.FromSeconds(5));
            port = await _endpoint.Ready.WaitAsync(cts.Token);
        }
        catch (OperationCanceledException) when (!context.RequestAborted.IsCancellationRequested)
        {
            _logger.LogWarning(
                "Console WS proxy gave up waiting for worker endpoint " +
                "(5s) on {Path}", context.Request.Path);
            context.Response.StatusCode = StatusCodes.Status503ServiceUnavailable;
            return;
        }

        // Open the upstream WebSocket BEFORE accepting the browser's
        // upgrade. That way, if the worker is down or rejecting us, we
        // can return a clean 502 without ever upgrading the customer's
        // connection — at which point we can no longer set a status code.
        using var upstream = new ClientWebSocket();
        // Carry subprotocol(s) the SPA requested. WebSocketAcceptContext
        // would otherwise pick the first one; ClientWebSocket needs them
        // declared up front.
        foreach (var sp in context.WebSockets.WebSocketRequestedProtocols)
        {
            upstream.Options.AddSubProtocol(sp);
        }

        var targetUri = new Uri(
            $"ws://127.0.0.1:{port}{context.Request.Path}{context.Request.QueryString}");

        try
        {
            using var connectCts = CancellationTokenSource.CreateLinkedTokenSource(
                context.RequestAborted);
            connectCts.CancelAfter(TimeSpan.FromSeconds(5));
            await upstream.ConnectAsync(
                targetUri, WorkerWebSocketInvoker, connectCts.Token);
        }
        catch (Exception ex) when (
            ex is WebSocketException or OperationCanceledException or HttpRequestException)
        {
            _logger.LogWarning(ex,
                "Console WS proxy upstream connect failed: {Path}",
                context.Request.Path);
            // Browser's connection has not been upgraded yet — we can
            // still write a status code. 502 matches the HTTP proxy
            // semantics for an unreachable upstream.
            if (!context.Response.HasStarted)
            {
                context.Response.StatusCode = StatusCodes.Status502BadGateway;
            }
            return;
        }

        WebSocket downstream;
        try
        {
            // Negotiate the subprotocol the upstream actually picked
            // (if any). Passing null tells AcceptWebSocketAsync to send
            // no Sec-WebSocket-Protocol header back.
            var acceptContext = new WebSocketAcceptContext
            {
                SubProtocol = upstream.SubProtocol,
            };
            downstream = await context.WebSockets.AcceptWebSocketAsync(
                acceptContext);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Console WS proxy downstream accept failed: {Path}",
                context.Request.Path);
            // Upstream is still open — close it cleanly.
            try
            {
                await upstream.CloseAsync(
                    WebSocketCloseStatus.EndpointUnavailable,
                    "downstream-accept-failed",
                    CancellationToken.None);
            }
            catch { /* best-effort */ }
            return;
        }

        try
        {
            await PumpBidirectionalAsync(
                downstream, upstream, context.RequestAborted);
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex,
                "Console WS proxy pump terminated abnormally: {Path}",
                context.Request.Path);
        }
        finally
        {
            downstream.Dispose();
        }
    }

    // Bidirectional frame pump. Each direction runs as its own task;
    // when one side closes (cleanly or otherwise) we propagate the
    // close to the other side and unblock the remaining task.
    //
    // Buffer size matches Kestrel's default WS receive buffer (4 KiB).
    // Log frames from the worker are <512 B in practice; stats frames
    // are <2 KiB. Larger frames are reassembled across reads.
    private static async Task PumpBidirectionalAsync(
        WebSocket downstream,
        WebSocket upstream,
        CancellationToken hostCt)
    {
        const int BufferSize = 4096;

        async Task PumpAsync(
            WebSocket from, WebSocket to, string direction, CancellationToken ct)
        {
            var buf = new byte[BufferSize];
            try
            {
                while (from.State == WebSocketState.Open ||
                       from.State == WebSocketState.CloseSent)
                {
                    WebSocketReceiveResult result;
                    try
                    {
                        result = await from.ReceiveAsync(
                            new ArraySegment<byte>(buf), ct);
                    }
                    catch (OperationCanceledException)
                    {
                        return;
                    }
                    catch (WebSocketException)
                    {
                        return;
                    }

                    if (result.MessageType == WebSocketMessageType.Close)
                    {
                        if (to.State == WebSocketState.Open ||
                            to.State == WebSocketState.CloseReceived)
                        {
                            try
                            {
                                await to.CloseOutputAsync(
                                    from.CloseStatus ?? WebSocketCloseStatus.NormalClosure,
                                    from.CloseStatusDescription,
                                    CancellationToken.None);
                            }
                            catch { /* best-effort */ }
                        }
                        return;
                    }

                    if (to.State != WebSocketState.Open &&
                        to.State != WebSocketState.CloseReceived)
                    {
                        return;
                    }

                    try
                    {
                        await to.SendAsync(
                            new ArraySegment<byte>(buf, 0, result.Count),
                            result.MessageType,
                            result.EndOfMessage,
                            CancellationToken.None);
                    }
                    catch (WebSocketException)
                    {
                        return;
                    }
                }
            }
            catch
            {
                // Last-resort: swallow so the other direction can finish.
            }
        }

        var d2u = PumpAsync(downstream, upstream, "client->worker", hostCt);
        var u2d = PumpAsync(upstream, downstream, "worker->client", hostCt);
        await Task.WhenAny(d2u, u2d);

        // Encourage the other direction to finish quickly. If either
        // socket is still open at this point, the partner pump is
        // probably blocked on a ReceiveAsync that will never produce
        // more data — cancel by closing the socket.
        try
        {
            if (upstream.State == WebSocketState.Open)
            {
                await upstream.CloseAsync(
                    WebSocketCloseStatus.NormalClosure, "peer-closed",
                    CancellationToken.None);
            }
        }
        catch { /* best-effort */ }
        try
        {
            if (downstream.State == WebSocketState.Open)
            {
                await downstream.CloseAsync(
                    WebSocketCloseStatus.NormalClosure, "peer-closed",
                    CancellationToken.None);
            }
        }
        catch { /* best-effort */ }

        await Task.WhenAll(d2u, u2d);
    }

    private async Task ProxyToWorkerAsync(HttpContext context)
    {
        int port;
        try
        {
            // Bounded wait: if the worker hasn't published its port
            // after 5 s, give up rather than holding the connection
            // open indefinitely.
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(
                context.RequestAborted);
            cts.CancelAfter(TimeSpan.FromSeconds(5));
            port = await _endpoint.Ready.WaitAsync(cts.Token);
        }
        catch (OperationCanceledException) when (!context.RequestAborted.IsCancellationRequested)
        {
            _logger.LogWarning(
                "Console proxy gave up waiting for worker endpoint " +
                "(5s) on {Method} {Path}",
                context.Request.Method, context.Request.Path);
            context.Response.StatusCode = StatusCodes.Status503ServiceUnavailable;
            return;
        }

        var client = _httpClientFactory.CreateClient("PageSpeedConsoleProxy");
        var targetUri = new Uri(
            $"http://127.0.0.1:{port}{context.Request.Path}{context.Request.QueryString}");

        using var upstreamRequest = new HttpRequestMessage
        {
            RequestUri = targetUri,
            Method = new HttpMethod(context.Request.Method),
        };

        var hasBody = HttpMethods.IsPost(context.Request.Method)
            || HttpMethods.IsPut(context.Request.Method)
            || HttpMethods.IsPatch(context.Request.Method)
            || HttpMethods.IsDelete(context.Request.Method);

        if (hasBody)
        {
            // Wrap the request body so `upstreamRequest.Dispose()` (and
            // by extension `StreamContent.Dispose()`) does not close the
            // host's request body — Kestrel still owns the lifetime of
            // context.Request.Body and may need it for trailers / error
            // reporting after we return.
            upstreamRequest.Content = new StreamContent(
                new NonClosingStream(context.Request.Body));
        }

        foreach (var header in context.Request.Headers)
        {
            if (HopByHopHeaders.Contains(header.Key)) continue;
            // Try as a request header; if that fails (e.g. Content-Type
            // belongs on content), fall back to content header.
            var values = header.Value.ToArray();
            if (!upstreamRequest.Headers.TryAddWithoutValidation(header.Key, values))
            {
                upstreamRequest.Content?.Headers.TryAddWithoutValidation(
                    header.Key, values);
            }
        }

        HttpResponseMessage upstreamResponse;
        try
        {
            upstreamResponse = await client.SendAsync(
                upstreamRequest,
                HttpCompletionOption.ResponseHeadersRead,
                context.RequestAborted);
        }
        catch (HttpRequestException ex)
        {
            _logger.LogWarning(ex,
                "Console proxy upstream failed: {Method} {Path}",
                context.Request.Method, context.Request.Path);
            context.Response.StatusCode = StatusCodes.Status502BadGateway;
            return;
        }
        catch (TaskCanceledException ex) when (!context.RequestAborted.IsCancellationRequested)
        {
            _logger.LogWarning(ex,
                "Console proxy upstream timed out: {Method} {Path}",
                context.Request.Method, context.Request.Path);
            context.Response.StatusCode = StatusCodes.Status504GatewayTimeout;
            return;
        }

        try
        {
            context.Response.StatusCode = (int)upstreamResponse.StatusCode;

            foreach (var header in upstreamResponse.Headers)
            {
                if (HopByHopHeaders.Contains(header.Key)) continue;
                context.Response.Headers[header.Key] = header.Value.ToArray();
            }
            foreach (var header in upstreamResponse.Content.Headers)
            {
                if (HopByHopHeaders.Contains(header.Key)) continue;
                context.Response.Headers[header.Key] = header.Value.ToArray();
            }
            // Kestrel sets Transfer-Encoding/Content-Length itself.
            context.Response.Headers.Remove("transfer-encoding");

            await using var upstreamBody = await upstreamResponse.Content
                .ReadAsStreamAsync(context.RequestAborted);
            await upstreamBody.CopyToAsync(
                context.Response.Body, context.RequestAborted);
        }
        finally
        {
            upstreamResponse.Dispose();
        }
    }

    // Wraps a stream so that disposing the wrapper leaves the inner
    // stream open. Used to hand `context.Request.Body` to StreamContent
    // without StreamContent's Dispose() closing Kestrel's body stream.
    private sealed class NonClosingStream(Stream inner) : Stream
    {
        private readonly Stream _inner = inner;
        public override bool CanRead => _inner.CanRead;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => _inner.Length;
        public override long Position { get => _inner.Position; set => throw new NotSupportedException(); }
        public override void Flush() => _inner.Flush();
        public override int Read(byte[] buffer, int offset, int count) =>
            _inner.Read(buffer, offset, count);
        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken ct) =>
            _inner.ReadAsync(buffer, offset, count, ct);
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken ct = default) =>
            _inner.ReadAsync(buffer, ct);
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        protected override void Dispose(bool disposing) { /* intentionally not disposing inner */ }
    }
}

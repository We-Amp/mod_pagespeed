// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.HostFiltering;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Microsoft.IO;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// ASP.NET Core middleware that intercepts responses and applies
/// PageSpeed optimizations. Handles HTML (transform + cache), images
/// (cache + worker transcoding), CSS and JS (cache + worker minification).
/// Convention-based (singleton lifetime).
/// </summary>
public sealed class PageSpeedMiddleware
{
    // Characters that could cause header injection in Link headers.
    private static readonly SearchValues<char> UnsafeLinkChars =
        SearchValues.Create("\r\n<>;,\"\0");

    private static readonly RecyclableMemoryStreamManager MsPool = new(
        new RecyclableMemoryStreamManager.Options
        {
            BlockSize = 128 * 1024,
            LargeBufferMultiple = 1024 * 1024,
            MaximumBufferSize = 8 * 1024 * 1024,
        });

    // Default capability mask: Desktop/Identity = 0x08.
    private const uint DefaultMask = 0x08;

    // Transfer encoding bits (6-7) in capability mask.
    private const uint EncodingMask = 0xC0;
    private const uint EncodingIdentity = 0x00;
    private const uint EncodingGzip = 0x40;
    private const uint EncodingBrotli = 0x80;

    // Image format bits (0-1) in capability mask.
    private const uint FormatMask = 0x03;
    private const uint FormatOriginal = 0x00;
    private const uint FormatWebP = 0x01;
    private const uint FormatAvif = 0x02;
    private const uint FormatSvg = 0x03;

    // CSP-safe async-CSS loader served at its reserved same-origin path. The
    // native path is CONTENT-ADDRESSED (a loader-body change yields a new path),
    // so it CANNOT be a managed compile-time literal. We resolve it ONCE from the
    // native single source of truth at type-init (process startup, before any
    // request is served) and cache it in a static readonly field. The per-request
    // path probe then compares against this in-memory string and NEVER calls into
    // native code — a native symbol skew must not be able to fault all GET/HEAD
    // traffic. The native lib is loaded in-process, so this path is identical to
    // the one the transform emits into HTML by construction. If the native getter
    // is unavailable/empty at startup the type initializer throws (fail-fast at
    // boot — TypeInitializationException — not a silent per-request 500). The JS
    // BODY is fetched from the same native source lazily and only after the path
    // already matched, so a body-getter skew breaks only the loader route.
    private static readonly string AsyncCssLoaderPath = ResolveAsyncCssLoaderPath();

    private static string ResolveAsyncCssLoaderPath()
    {
        // ps_async_css_loader_path is already an exported, in-process symbol
        // (the JS-body sibling reads its neighbour), so this adds no new symbol.
        var path = NativePageSpeed.AsyncCssLoaderPath();
        if (string.IsNullOrEmpty(path))
        {
            throw new InvalidOperationException(
                "Native async-CSS loader path is empty; the loaded PageSpeed " +
                "native library is missing ps_async_css_loader_path or is " +
                "ABI-incompatible. Refusing to start with an unknown loader path.");
        }
        return path;
    }

    private static readonly Lazy<byte[]> AsyncCssLoaderJsBytes =
        new(() => Encoding.UTF8.GetBytes(NativePageSpeed.AsyncCssLoaderJs()));

    private readonly RequestDelegate _next;
    private readonly IOptionsMonitor<PageSpeedOptions> _options;
    private readonly IHtmlProcessor _htmlProcessor;
    private readonly IPageSpeedCache _cache;
    private readonly WorkerNotificationService _notifier;
    private readonly ILogger<PageSpeedMiddleware> _logger;
    private readonly IOptionsMonitor<HostFilteringOptions>? _hostFiltering;

    /// <summary>
    /// Creates a new PageSpeed middleware instance.
    /// </summary>
    public PageSpeedMiddleware(
        RequestDelegate next,
        IOptionsMonitor<PageSpeedOptions> options,
        IHtmlProcessor htmlProcessor,
        IPageSpeedCache cache,
        WorkerNotificationService notifier,
        ILogger<PageSpeedMiddleware> logger,
        IOptionsMonitor<HostFilteringOptions>? hostFiltering = null)
    {
        _next = next;
        _hostFiltering = hostFiltering;
        _options = options;
        _htmlProcessor = htmlProcessor;
        _cache = cache;
        _notifier = notifier;
        _logger = logger;

        var opts = options.CurrentValue;
        _logger.LogInformation(
            "PageSpeed cache_mode={Mode} css_max_age={Css}s image_max_age={Image}s",
            opts.CacheMode, opts.CssMaxAgeSeconds, opts.ImageMaxAgeSeconds);
    }

    /// <summary>
    /// Invokes the middleware for the given HTTP context.
    /// </summary>
    public async Task InvokeAsync(HttpContext context)
    {
        var opts = _options.CurrentValue;

        // Serve the CSP-safe async-CSS loader JS at its reserved same-origin
        // path (same single source of truth the nginx module serves). Static;
        // only pages whose stylesheets the transform deferred reference it.
        // Checked before the exclude/method gates so the loader is always
        // reachable when the middleware is enabled.
        if (opts.Enabled &&
            (HttpMethods.IsGet(context.Request.Method) ||
             HttpMethods.IsHead(context.Request.Method)) &&
            context.Request.Path.HasValue &&
            string.Equals(context.Request.Path.Value, AsyncCssLoaderPath,
                          StringComparison.Ordinal))
        {
            await ServeAsyncCssLoaderAsync(context);
            return;
        }

        if (!opts.Enabled || IsExcluded(context.Request, opts))
        {
            await _next(context);
            return;
        }

        // Bypass WebSocket upgrades.
        if (context.WebSockets.IsWebSocketRequest)
        {
            await _next(context);
            return;
        }

        // Only cache GET and HEAD requests. POST/PUT/DELETE responses
        // must not poison the cache for subsequent GET requests.
        if (!HttpMethods.IsGet(context.Request.Method) &&
            !HttpMethods.IsHead(context.Request.Method))
        {
            await _next(context);
            return;
        }

        // Build URL and hostname for cache lookups.
        // URL must be path+query only (no scheme, no host) to match the
        // format used by the nginx/Apache modules and expected by the worker.
        var url = string.Concat(
            context.Request.PathBase.Value,
            context.Request.Path.Value,
            context.Request.QueryString.Value);
        var hostname = context.Request.Host.Host;

        // Scheme for cache key partitioning ("http" or "https").
        var scheme = context.Request.Scheme;
        if (scheme != "http" && scheme != "https")
            scheme = "https";

        // Classify request into capability mask.
        uint mask = DefaultMask;
        try { mask = RequestClassifier.Classify(context.Request); }
        catch (Exception ex)
        {
            _logger.LogDebug(ex,
                "Request classification failed, using default mask");
        }

        // Read on the HIT, early-hints and record paths below.
        bool requestHasCookie = CookieRequestCacheRule.RequestHasCookie(context.Request);

        // ── CACHE HIT fast path ──────────────────────────────────
        // Skip cache on force-refresh (Ctrl+F5: Cache-Control: no-cache
        // or Pragma: no-cache).
        if (!IsForceRefresh(context.Request))
        {
            try
            {
                // ── Agent-markdown negotiation (OFF by default) ──
                // Mirror the nginx serve model: when the operator flag is on and
                // the request explicitly wants markdown,
                // read the worker-written entitlement and route the cache read
                // through the SINGLE audited native gate (ReadBestAgent). With
                // entitled=0 the gate behaves exactly like ReadBest (0x7C is never
                // eligible), so non-markdown / non-entitled requests are
                // unaffected. The native gate also enforces the content-hash
                // binding — a stale/unbound markdown is refused (null → HTML).
                // Rendered markdown is fetched from the host name, so it is
                // served only under a host AllowedHosts names.
                int agentEntitled = 0;
                if (opts.EnableAgentOptimize
                    && HostVouched(hostname)
                    && RequestClassifier.WantsAgentMarkdown(context.Request))
                {
                    try
                    {
                        agentEntitled =
                            _cache.ReadSharedConfigAgentEntitled() == 1 ? 1 : 0;
                    }
                    catch (Exception ex)
                    {
                        _logger.LogDebug(ex,
                            "Agent entitlement read failed for {Url}; serving HTML",
                            url);
                    }
                }

                using var cached = agentEntitled == 1
                    ? _cache.ReadBestAgent(url, hostname, scheme, mask, agentEntitled)
                    : _cache.ReadBest(url, hostname, scheme, mask);
                if (cached != null)
                {
                    // Did the gate hand us the markdown variant? (Only an entitled
                    // markdown request can reach here, and the binding gate already
                    // ran — so a 0x7C variant is fresh and bound.)
                    bool servedAgentMarkdown =
                        agentEntitled == 1 &&
                        (cached.Mask & 0xFF) == SentinelId.AgentMarkdown;

                    // The response headers the application set when the entry was
                    // stored ride along with it. An entry that cannot be served
                    // with them is treated as a miss and refetched.
                    // A request carrying a cookie is served only an entry the
                    // origin marked public or gave one s-maxage above zero
                    // (CookieRequestCacheRule); otherwise it goes to the
                    // application.
                    var stored = requestHasCookie &&
                                 !StoredEntrySharedWithCookieRequest(
                                     cached, url, hostname, scheme, servedAgentMarkdown)
                        ? new StoredHeaders(false, null)
                        : ResolveStoredHeaders(
                            cached, url, hostname, scheme, servedAgentMarkdown);
                    if (stored.Usable)
                    {
                        if (ServeCacheHit(context, cached, url, hostname, scheme, mask,
                                          opts, servedAgentMarkdown, stored.Fields))
                        {
                            await WriteBorrowedContentAsync(context, cached);
                        }
                        return;
                    }
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex,
                    "Cache read failed for {Url}, falling through to origin",
                    url);
            }
        }

        // ── Early Hints for MISS path ──────────────────────────────
        // If cached early hints exist from a prior request, add Link
        // headers now so they ship with the response start. This lets
        // browsers begin fetching critical sub-resources during origin
        // processing.
        // The header lines on the response when this middleware is entered
        // belong to middleware ahead of it, not to the origin response: they
        // are not stored with the entry.
        var capture = new MissCapture(context.Response);
        var hintLinksBefore = context.Response.Headers.Link.ToArray();
        AddEarlyHintLinkHeaders(context, url, hostname, scheme, requestHasCookie);
        capture.OwnLinks.AddRange(SubtractLines(
            context.Response.Headers.Link.ToArray(), hintLinksBefore));

        // Headers an application adds in OnStarting callbacks only exist once
        // the response starts. Callbacks run last-registered first, so this
        // one (registered before the application runs) runs after all of
        // theirs and sees the final header set.
        context.Response.OnStarting(
            static state => { ((MissCapture)state).Capture(); return Task.CompletedTask; },
            capture);

        // ── CACHE MISS — proxy to origin ─────────────────────────
        var originalBody = context.Response.Body;
        using var buffer = MsPool.GetStream("PageSpeed.Buffer");
        context.Response.Body = buffer;

        try
        {
            await _next(context);
        }
        catch
        {
            context.Response.Body = originalBody;
            throw;
        }

        // Clear Content-Length -- we may change the body size.
        context.Response.ContentLength = null;
        context.Response.Body = originalBody;

        // Skip non-2xx or empty responses.
        if (context.Response.StatusCode < 200 ||
            context.Response.StatusCode >= 300 ||
            buffer.Length == 0)
        {
            buffer.Seek(0, SeekOrigin.Begin);
            await buffer.CopyToAsync(originalBody, context.RequestAborted);
            return;
        }

        // [Critical #1] Skip caching for uncacheable responses:
        // no-store, private, or Set-Cookie.
        var cacheability = ClassifyCacheability(context.Response);
        if (cacheability == ResponseCacheability.Uncacheable)
        {
            buffer.Seek(0, SeekOrigin.Begin);
            await buffer.CopyToAsync(originalBody, context.RequestAborted);
            return;
        }

        // Only the response to an anonymous request is stored. Evaluated now,
        // after the application ran, so authentication and endpoint selection
        // placed after this middleware are visible.
        var record = IsAnonymousRequest(context);

        // Classify response content type (managed, no P/Invoke needed).
        var responseContentType = context.Response.ContentType;
        var psContentType = ClassifyResponseContentType(responseContentType);
        var cacheControl = context.Response.Headers.CacheControl.ToString();

        // A response to a request carrying a cookie is stored only when the
        // origin opted in (CookieRequestCacheRule): a page needs public or
        // one s-maxage above zero; a stylesheet, script or image needs a
        // freshness lifetime the origin stated.
        if (record && requestHasCookie)
            record = CookieRequestMayStore(context.Response, cacheControl);

        switch (psContentType)
        {
            case PageSpeedContentType.Html:
                await HandleHtmlMiss(
                    context, originalBody, buffer, opts,
                    url, hostname, scheme, mask, cacheability,
                    cacheControl, capture, record);
                break;

            case PageSpeedContentType.Image:
            case PageSpeedContentType.Css:
            case PageSpeedContentType.Js:
                if (!record)
                {
                    // Served as the application served it; not stored.
                    buffer.Seek(0, SeekOrigin.Begin);
                    await buffer.CopyToAsync(
                        originalBody, context.RequestAborted);
                    break;
                }
                await HandleAssetMiss(
                    context, originalBody, buffer, opts,
                    url, hostname, scheme, mask,
                    psContentType, responseContentType,
                    cacheability, cacheControl, capture);
                break;

            default:
                // Not a cacheable type — pass through.
                buffer.Seek(0, SeekOrigin.Begin);
                await buffer.CopyToAsync(
                    originalBody, context.RequestAborted);
                break;
        }
    }

    /// <summary>
    /// Serves a cache HIT for any content type. Sets appropriate headers
    /// including Content-Type (with image format negotiation), encoding,
    /// Vary, Cache-Control, ETag, Last-Modified, and X-PageSpeed.
    /// Returns true if the body should be written (200), false for 304.
    /// </summary>
    /// <summary>
    /// Should a worker notification for this request carry the
    /// agent_request bit (asking the worker to render markdown)? The same gate
    /// as the serve negotiation — OFF by default, the request explicitly wants
    /// markdown, AND the worker-written entitlement is set.
    /// Short-circuits before the native call for non-agent traffic; fail-closed.
    /// </summary>
    private bool ShouldRequestAgentRender(HttpContext context, PageSpeedOptions opts)
    {
        if (!opts.EnableAgentOptimize) return false;
        try
        {
            return RequestClassifier.WantsAgentMarkdown(context.Request)
                && _cache.ReadSharedConfigAgentEntitled() == 1;
        }
        catch
        {
            return false;
        }
    }

    // Lease renewal for the zero-copy HIT write.
    // cached.ContentMemory borrows Cyclone mmap bytes; the async transfer to
    // a slow client can hold that borrow far past the read lease's 3.75s
    // guaranteed floor (3/4 of the default 5s lease). Chunking the write
    // gives us a renewal point whenever transport backpressure returns
    // control between chunks: we re-stamp the lease at a <= 3s cadence.
    // Residuals (documented, accepted): a client that stalls the transport
    // for minutes *within* a single chunk await gives no renewal point, and
    // renewal itself stops protecting past Cyclone's 60s anti-starvation
    // wrap ceiling — either way a multi-minute transfer is unprotected
    // again past 60s (pre-lease exposure, now bounded and observable via
    // the cache's wraps_forced_past_lease counter).
    private const int LeaseWriteChunkBytes = 128 * 1024;
    private static readonly TimeSpan LeaseRenewInterval =
        TimeSpan.FromSeconds(3);

    private static async Task WriteBorrowedContentAsync(
        HttpContext context, IReadResult cached)
    {
        var content = cached.ContentMemory;
        if (content.Length <= LeaseWriteChunkBytes)
        {
            // Typical sizes: single write, completes well inside the
            // initial lease stamped by the read itself.
            await context.Response.Body.WriteAsync(
                content, context.RequestAborted);
            return;
        }

        var sinceRenew = System.Diagnostics.Stopwatch.StartNew();
        for (int offset = 0; offset < content.Length;
             offset += LeaseWriteChunkBytes)
        {
            if (sinceRenew.Elapsed >= LeaseRenewInterval)
            {
                cached.RenewLease();
                sinceRenew.Restart();
            }
            int len = Math.Min(LeaseWriteChunkBytes, content.Length - offset);
            await context.Response.Body.WriteAsync(
                content.Slice(offset, len), context.RequestAborted);
        }
    }

    private bool ServeCacheHit(
        HttpContext context, IReadResult cached,
        string url, string hostname, string scheme, uint mask,
        PageSpeedOptions opts, bool servedAgentMarkdown = false,
        IReadOnlyList<KeyValuePair<string, string>>? storedHeaders = null)
    {
        var cachedMask = cached.Mask;
        var cachedType = cached.ContentType;
        var content = cached.ContentMemory;

        // Determine Content-Type. The agent markdown variant is served
        // as text/markdown at the same URL. Otherwise, for images the worker may
        // have transcoded to a different format (WebP, AVIF, SVG).
        string? ct;
        if (servedAgentMarkdown)
        {
            ct = "text/markdown; charset=utf-8";
        }
        else
        {
            ct = cachedType == PageSpeedContentType.Image
                ? GetImageContentType(cachedMask, cached.OriginContentType)
                : cached.OriginContentType;
            ct ??= cachedType switch
            {
                PageSpeedContentType.Html => "text/html; charset=utf-8",
                PageSpeedContentType.Css => "text/css; charset=utf-8",
                PageSpeedContentType.Js => "application/javascript; charset=utf-8",
                _ => "application/octet-stream",
            };
        }

        context.Response.StatusCode = 200;

        // The application's own response headers first, so the 200 and the
        // 304 below both carry them. Headers this method builds itself follow.
        if (storedHeaders != null)
            ReplayStoredHeaders(context.Response.Headers, storedHeaders);

        context.Response.ContentType = ct;
        context.Response.ContentLength = content.Length;
        context.Response.Headers["X-PageSpeed"] = "HIT";

        // Serve-time bandwidth savings: mirror the nginx front-end,
        // which records right after it adds the HIT header. Gate is
        // byte-for-byte the nginx gate — worker-processed AND a non-zero origin
        // size (front-end-written originals carry origin_content_length == 0).
        // Recorded here (not on the 304 paths below) because a subsequent 304
        // still represents a validated cache hit, exactly as nginx counts when
        // it adds the HIT header.
        // Markdown HITs are excluded from bandwidth-saved accounting —
        // that story is about optimized assets, not the rendered-DOM variant.
        if (!servedAgentMarkdown &&
            cached.IsWorkerProcessed && cached.OriginContentLength > 0)
            // Pass the served variant's mask so SVG image variants (format bits
            // == SVG) also tick svg.served, mirroring nginx.
            _cache.RecordServeHit(cachedType, cached.OriginContentLength, (ulong)content.Length, cachedMask);

        // Cache-Control on HIT responses.
        // Note: origin private/no-store responses never reach here —
        // they are filtered by ClassifyCacheability() upstream.
        if (servedAgentMarkdown)
        {
            // The markdown variant is anonymous-view-only and must
            // never be stored by a shared cache.
            context.Response.Headers.CacheControl = "private, no-cache";
        }
        else if (cachedType == PageSpeedContentType.Html)
        {
            // HTML always gets no-cache in both modes.
            context.Response.Headers.CacheControl = "no-cache";
        }
        else
        {
            var maxAge = GetMaxAgeForType(cachedType, opts);
            context.Response.Headers.CacheControl =
                BuildAssetCacheControl(
                    opts.CacheMode, maxAge,
                    cached.OriginCcFlags);
        }

        // Age and Last-Modified headers from cache insertion time.
        var insertedAt = cached.CacheInsertedAt;
        if (insertedAt > 0)
        {
            var age = DateTimeOffset.UtcNow.ToUnixTimeSeconds() - insertedAt;
            if (age > 0)
                context.Response.Headers["Age"] = age.ToString();

            var lastModified = DateTimeOffset.FromUnixTimeSeconds(insertedAt);
            context.Response.Headers["Last-Modified"] =
                lastModified.ToString("R");
        }

        // Content-Encoding for compressed variants.
        var encoding = cachedMask & EncodingMask;
        if (encoding == EncodingGzip)
            context.Response.Headers.ContentEncoding = "gzip";
        else if (encoding == EncodingBrotli)
            context.Response.Headers.ContentEncoding = "br";

        // Vary headers per content type.
        SetVaryHeaders(context, cachedType);

        if (servedAgentMarkdown)
        {
            // Accept MUST participate in Vary so an intermediary can
            // never serve the markdown body to a non-markdown request, and the
            // rendered-DOM markdown must stay out of search indexes. Set
            // deterministically (the 0x7C variant is typed kOther, so
            // SetVaryHeaders leaves Vary unset for it).
            context.Response.Headers.Vary = "Accept, Accept-Encoding";
            context.Response.Headers["X-Robots-Tag"] = "noindex";
        }

        // Link preload/preconnect headers (HTML only; markdown has no browser
        // sub-resources).
        if (cachedType == PageSpeedContentType.Html && !servedAgentMarkdown)
            AddLinkHeadersFromCache(context, url, hostname, scheme);

        // Re-notify worker if cached entry needs revalidation.
        if (cached.NeedsRevalidation &&
            !WithholdHtmlNotify(cachedType, hostname))
            if (!_notifier.TryNotify(url, hostname, scheme, cachedType, mask,
                                     ShouldRequestAgentRender(context, opts),
                                     hostVouched: HostVouched(hostname)))
                _logger.LogDebug("TryNotify failed for revalidation: {Url}", url);

        // ETag derived from the served bytes, so two different payloads
        // never share a tag and identical content keeps its tag across
        // re-insertions.
        var etag = ComputeContentETag(content.Span);
        context.Response.Headers.ETag = etag;

        // ── Conditional request: If-None-Match ──────────────────
        var ifNoneMatch = context.Request.Headers.IfNoneMatch.ToString();
        if (!string.IsNullOrEmpty(ifNoneMatch))
        {
            if (MatchesETag(ifNoneMatch, etag))
            {
                context.Response.StatusCode = StatusCodes.Status304NotModified;
                context.Response.ContentLength = null;
                return false;
            }
        }

        // ── Conditional request: If-Modified-Since ──────────────
        // Ignored when If-None-Match is present (RFC 9110 section 13.1.3).
        var ifModifiedSince = context.Request.Headers.IfModifiedSince.ToString();
        if (string.IsNullOrEmpty(ifNoneMatch) &&
            !string.IsNullOrEmpty(ifModifiedSince) &&
            cached.CacheInsertedAt > 0)
        {
            if (DateTimeOffset.TryParse(ifModifiedSince, out var since))
            {
                var insertedAtDto = DateTimeOffset.FromUnixTimeSeconds(
                    cached.CacheInsertedAt);
                if (insertedAtDto <= since)
                {
                    context.Response.StatusCode = StatusCodes.Status304NotModified;
                    context.Response.ContentLength = null;
                    return false;
                }
            }
        }

        return true;
    }

    /// <summary>
    /// Returns the max-age value (seconds) for the given content type
    /// based on per-type options. CSS and JS share the same setting.
    /// </summary>
    private static int GetMaxAgeForType(
        PageSpeedContentType type, PageSpeedOptions opts)
        => type switch
        {
            PageSpeedContentType.Css or PageSpeedContentType.Js => opts.CssMaxAgeSeconds,
            PageSpeedContentType.Image => opts.ImageMaxAgeSeconds,
            _ => opts.CssMaxAgeSeconds,
        };

    /// <summary>
    /// Builds the Cache-Control header for a non-HTML cache HIT response.
    /// Matches the nginx-side BuildCacheControlHeader contract (lib/cache/
    /// cache_control_header.cc, post-PR #252): in aggressive mode,
    /// stale-if-error is only emitted when the origin permits stale
    /// serving. When the origin sent must-revalidate / no-cache /
    /// proxy-revalidate / s-maxage, RFC 9111 §4.2.4 forbids stale
    /// serving and §5.2.2.11 makes stale-if-error meaningless — so we
    /// suppress it and propagate must-revalidate instead.
    /// </summary>
    internal static string BuildAssetCacheControl(
        CacheMode mode, int maxAge, ushort originCcFlags)
    {
        if (mode == CacheMode.Safe)
            return $"max-age={maxAge}, must-revalidate";

        // Aggressive mode. For a private cache (ASP.NET in-process),
        // proxy-revalidate and s-maxage are ignored per RFC 9111 §5.2.2.9-10.
        bool revalidationRequired =
            (originCcFlags & (CacheControlCcFlags.NoCache | CacheControlCcFlags.MustRevalidate)) != 0;

        if (revalidationRequired)
            return $"public, max-age={maxAge}, must-revalidate";

        return $"public, max-age={maxAge}, stale-if-error=86400";
    }

    /// <summary>
    /// Returns true when the origin Cache-Control header contains a
    /// directive that forbids serving stale responses per RFC 9111
    /// §4.2.4: must-revalidate, no-cache, proxy-revalidate, or
    /// s-maxage. Mirrors the nginx-side `revalidation_required` flag
    /// <summary>
    /// For images, derives Content-Type from format bits in the
    /// capability mask. The worker writes transcoded variants with
    /// format bits set to WebP (01), AVIF (10), or SVG (11).
    /// </summary>
    private static string GetImageContentType(
        uint mask, string? originContentType)
    {
        return (mask & FormatMask) switch
        {
            FormatWebP => "image/webp",
            FormatAvif => "image/avif",
            FormatSvg => "image/svg+xml",
            _ => originContentType ?? "image/jpeg",
        };
    }

    /// <summary>
    /// Sets Vary headers appropriate for the content type.
    /// </summary>
    private static void SetVaryHeaders(
        HttpContext context, PageSpeedContentType type,
        MissCapture? capture = null)
    {
        // Merged with any Vary already on the response (the application's own,
        // or one replayed from the stored headers): the tokens of both apply.
        string? ours = type switch
        {
            PageSpeedContentType.Image => "Accept, Save-Data, User-Agent",
            PageSpeedContentType.Html => "Accept-Encoding, User-Agent",
            PageSpeedContentType.Css or PageSpeedContentType.Js => "Accept-Encoding",
            _ => null,
        };
        if (ours == null) return;
        var added = MergeVary(context.Response.Headers, ours);
        capture?.OwnVaryTokens.AddRange(added);
    }

    /// <summary>
    /// Sets <c>Vary</c> to the union of the tokens already on the response and
    /// those in <paramref name="additions"/> (case-insensitive, existing order
    /// first). A <c>*</c> anywhere makes the whole value <c>*</c>.
    /// </summary>
    internal static List<string> MergeVary(IHeaderDictionary headers, string additions)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var merged = new List<string>();
        var added = new List<string>();
        bool star = false;

        void Collect(string? list, bool isAddition)
        {
            if (string.IsNullOrEmpty(list)) return;
            foreach (var raw in list.Split(','))
            {
                var token = raw.Trim();
                if (token.Length == 0) continue;
                if (token == "*") star = true;
                if (seen.Add(token))
                {
                    merged.Add(token);
                    if (isAddition) added.Add(token);
                }
            }
        }

        foreach (var line in headers.Vary) Collect(line, false);
        Collect(additions, true);

        if (merged.Count == 0) return added;
        headers.Vary = star ? "*" : string.Join(", ", merged);
        return added;
    }

    // Header names the serve path builds itself, or that belong to one
    // connection or one client. The stored block never holds them; a block
    // that does is not trusted.
    private static readonly HashSet<string> ServeBuiltHeaders =
        new(StringComparer.OrdinalIgnoreCase)
        {
            "Cache-Control", "Content-Type", "Content-Length", "Content-Encoding",
            "Content-Range", "ETag", "Last-Modified", "Expires", "Date", "Age",
            "Server", "Accept-Ranges", "X-PageSpeed",
            "Set-Cookie", "Set-Cookie2", "Authorization", "WWW-Authenticate",
            "Proxy-Authenticate", "Proxy-Authorization",
            "Connection", "Keep-Alive", "Transfer-Encoding", "TE", "Trailer",
            "Upgrade",
        };

    private readonly record struct StoredHeaders(
        bool Usable, IReadOnlyList<KeyValuePair<string, string>>? Fields);

    /// <summary>
    /// May <paramref name="cached"/> be served to a request carrying a cookie
    /// (CookieRequestCacheRule.CookieRequestMayServe)? Stylesheets, scripts
    /// and images: yes. Anything else: only when the stored entry carries the
    /// origin's public mark or one s-maxage above zero. The markdown copy
    /// carries no Cache-Control of its own; the recorded page decides for it,
    /// and without a recorded page it is not served.
    /// </summary>
    private bool StoredEntrySharedWithCookieRequest(
        IReadResult cached, string url, string hostname, string scheme,
        bool servedAgentMarkdown)
    {
        bool shared = servedAgentMarkdown
            ? StoredPageSharedWithCookieRequest(url, hostname, scheme)
            : CookieRequestCacheRule.CookieRequestMayServe(
                requestHasCookie: true,
                CookieRequestCacheRule.StoredEntryIsStaticAsset(
                    cached.OriginContentType, cached.ContentType),
                CookieRequestCacheRule.HtmlExplicitlyShared(
                    cached.OriginCcFlags, cached.OriginSMaxage));
        if (!shared)
            _logger.LogDebug(
                "Request for {Url} carries a cookie and the stored entry is not marked public or s-maxage; refetching",
                url);
        return shared;
    }

    /// <summary>
    /// Does the page recorded for this URL carry the origin's public or
    /// s-maxage mark? False when there is no recorded page or it cannot be
    /// read.
    /// </summary>
    private bool StoredPageSharedWithCookieRequest(
        string url, string hostname, string scheme)
    {
        try
        {
            using var page = _cache.ReadAlternate(url, hostname, scheme, (byte)DefaultMask);
            return page != null &&
                   CookieRequestCacheRule.CookieRequestMayServe(
                       requestHasCookie: true, staticAsset: false,
                       CookieRequestCacheRule.HtmlExplicitlyShared(
                           page.OriginCcFlags, page.OriginSMaxage));
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "Reading the recorded page for {Url} failed", url);
            return false;
        }
    }

    /// <summary>
    /// Decides how a cache entry is served with respect to the origin's
    /// response headers: with a stored block to replay, with nothing to
    /// replay, or not at all (treated as a miss).
    /// </summary>
    private StoredHeaders ResolveStoredHeaders(
        IReadResult cached, string url, string hostname, string scheme,
        bool servedAgentMarkdown)
    {
        // The markdown rendering is built from fresh metadata; it carries
        // none of the origin's headers.
        if (servedAgentMarkdown) return new(true, null);

        var flags = cached.Flags;
        if ((flags & CacheFlags.HeadersClassified) == 0)
        {
            // Recorded before the origin's headers were kept with the entry.
            _logger.LogDebug(
                "Cache entry for {Url} has no stored response headers; refetching",
                url);
            return new(false, null);
        }
        if ((flags & CacheFlags.AnonymousRecorded) == 0)
        {
            // Not recorded under the anonymous-request rule.
            _logger.LogDebug(
                "Cache entry for {Url} was not recorded from an anonymous request; refetching",
                url);
            return new(false, null);
        }
        if ((flags & CacheFlags.HeadersSidecarRequired) == 0)
            return new(true, null);

        try
        {
            using var block = _cache.ReadAlternate(
                url, hostname, scheme, SentinelId.HeadersSidecar);
            if (block == null ||
                !HeadersSidecar.TryParse(block.ContentMemory.Span, out var fields) ||
                fields.Count == 0)
            {
                _logger.LogDebug(
                    "Stored response headers for {Url} are missing; refetching", url);
                return new(false, null);
            }
            foreach (var field in fields)
            {
                if (ServeBuiltHeaders.Contains(field.Key))
                {
                    _logger.LogWarning(
                        "Stored response headers for {Url} hold {Name}; refetching",
                        url, field.Key);
                    return new(false, null);
                }
            }
            return new(true, fields);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Reading stored response headers failed for {Url}; refetching", url);
            return new(false, null);
        }
    }

    /// <summary>
    /// Adds the stored headers to the response. A line already present with
    /// the same value (set by middleware ahead of this one) is not repeated;
    /// <c>Vary</c> is merged with what the response already has.
    /// </summary>
    private static void ReplayStoredHeaders(
        IHeaderDictionary headers,
        IReadOnlyList<KeyValuePair<string, string>> stored)
    {
        foreach (var group in stored.GroupBy(f => f.Key, StringComparer.OrdinalIgnoreCase))
        {
            if (group.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase))
            {
                foreach (var f in group) MergeVary(headers, f.Value);
                continue;
            }
            // The stored lines replace whatever the response already has under
            // the name: the response then carries exactly the stored value.
            headers[group.Key] = group.Select(f => f.Value).ToArray();
        }
    }

    /// <summary>
    /// What the origin response carries for one cache miss: the header lines
    /// present when the middleware was entered, the lines it adds itself, and
    /// the final header set once the response starts.
    /// </summary>
    private sealed class MissCapture
    {
        private readonly HttpResponse _response;
        private readonly List<KeyValuePair<string, string>> _entry = new();
        private readonly HashSet<string> _entryVary = new(StringComparer.OrdinalIgnoreCase);

        public MissCapture(HttpResponse response)
        {
            _response = response;
            foreach (var h in response.Headers)
                foreach (var v in h.Value)
                {
                    if (v == null) continue;
                    if (h.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase))
                        foreach (var t in v.Split(','))
                        {
                            if (t.Trim().Length > 0) _entryVary.Add(t.Trim());
                        }
                    else
                        _entry.Add(new(h.Key, v));
                }
        }

        /// <summary>Link lines this middleware added (early hints, preloads).</summary>
        public List<string> OwnLinks { get; } = new();

        /// <summary>Vary tokens this middleware added.</summary>
        public List<string> OwnVaryTokens { get; } = new();

        private List<KeyValuePair<string, string>>? _captured;
        private bool _uncacheable;

        /// <summary>The origin's header lines (null until captured).</summary>
        public IReadOnlyList<KeyValuePair<string, string>> Headers
        {
            get { Capture(); return _captured!; }
        }

        /// <summary>True when the final headers make the response uncacheable.</summary>
        public bool Uncacheable
        {
            get { Capture(); return _uncacheable; }
        }

        /// <summary>
        /// The cacheability of the response as it is now. Called after the body
        /// was written, when every OnStarting callback, including those of
        /// middleware ahead of this one, has run.
        /// </summary>
        public bool UncacheableNow =>
            ClassifyCacheability(_response) == ResponseCacheability.Uncacheable;

        public void Capture()
        {
            if (_captured != null) return;
            var entry = new List<KeyValuePair<string, string>>(_entry);
            var links = new List<string>(OwnLinks);
            var varyTokens = new List<string>();
            var captured = new List<KeyValuePair<string, string>>();
            foreach (var header in _response.Headers)
            {
                bool isVary = header.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase);
                bool isLink = header.Key.Equals("Link", StringComparison.OrdinalIgnoreCase);
                if (header.Key.Equals("X-PageSpeed", StringComparison.OrdinalIgnoreCase))
                    continue;
                foreach (var value in header.Value)
                {
                    if (value == null) continue;
                    if (isVary)
                    {
                        foreach (var raw in value.Split(','))
                        {
                            var t = raw.Trim();
                            if (t.Length == 0 || _entryVary.Contains(t)) continue;
                            if (OwnVaryTokens.Contains(t, StringComparer.OrdinalIgnoreCase)) continue;
                            if (!varyTokens.Contains(t, StringComparer.OrdinalIgnoreCase))
                                varyTokens.Add(t);
                        }
                        continue;
                    }
                    int e = entry.FindIndex(x =>
                        x.Key.Equals(header.Key, StringComparison.OrdinalIgnoreCase) &&
                        string.Equals(x.Value, value, StringComparison.Ordinal));
                    if (e >= 0) { entry.RemoveAt(e); continue; }
                    if (isLink)
                    {
                        int l = links.IndexOf(value);
                        if (l >= 0) { links.RemoveAt(l); continue; }
                    }
                    captured.Add(new(header.Key, value));
                }
            }
            if (varyTokens.Count > 0)
                captured.Add(new("Vary", string.Join(", ", varyTokens)));
            _uncacheable = ClassifyCacheability(_response) == ResponseCacheability.Uncacheable;
            _captured = captured;
        }
    }

    /// <summary>True when the response's own Vary names Accept.</summary>
    private static bool VariesOnAccept(IReadOnlyList<KeyValuePair<string, string>> headers)
    {
        foreach (var h in headers)
        {
            if (!h.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase)) continue;
            foreach (var token in h.Value.Split(','))
                if (token.Trim().Equals("Accept", StringComparison.OrdinalIgnoreCase))
                    return true;
        }
        return false;
    }

    /// <summary>Lines in <paramref name="after"/> that are not in <paramref name="before"/> (multiset difference).</summary>
    private static List<string> SubtractLines(string?[] after, string?[] before)
    {
        var remaining = new List<string?>(before);
        var added = new List<string>();
        foreach (var line in after)
        {
            if (line == null) continue;
            int i = remaining.IndexOf(line);
            if (i >= 0) remaining.RemoveAt(i);
            else added.Add(line);
        }
        return added;
    }

    /// <summary>
    /// Whether the request was anonymous as far as the application can tell:
    /// no <c>Authorization</c> or <c>Proxy-Authorization</c> header, no
    /// authenticated user, and an endpoint without an authorization
    /// requirement (<see cref="IAuthorizeData"/>, an
    /// <see cref="AuthorizationPolicy"/> or
    /// <see cref="IAuthorizationRequirementData"/> metadata), or one that
    /// allows anonymous access. Only the response
    /// to such a request is stored in the cache. Called after the rest of
    /// the pipeline has run.
    /// </summary>
    internal static bool IsAnonymousRequest(HttpContext context)
    {
        var headers = context.Request.Headers;
        if (headers.ContainsKey("Authorization") ||
            headers.ContainsKey("Proxy-Authorization"))
            return false;

        var user = context.User;
        if (user != null)
        {
            foreach (var identity in user.Identities)
            {
                if (identity.IsAuthenticated) return false;
            }
        }

        var metadata = context.GetEndpoint()?.Metadata;
        if (metadata != null &&
            (metadata.GetMetadata<IAuthorizeData>() != null ||
             metadata.GetMetadata<AuthorizationPolicy>() != null ||
             metadata.GetMetadata<IAuthorizationRequirementData>() != null) &&
            metadata.GetMetadata<IAllowAnonymous>() == null)
            return false;

        return true;
    }

    /// <summary>
    /// Handles an HTML cache MISS: transforms the HTML and, when
    /// <paramref name="record"/> is set, caches it, writes early hints, and
    /// notifies the worker. Otherwise the transformed page is served to this
    /// client only.
    /// </summary>
    private async Task HandleHtmlMiss(
        HttpContext context, Stream originalBody,
        RecyclableMemoryStream buffer, PageSpeedOptions opts,
        string url, string hostname, string scheme, uint mask,
        ResponseCacheability cacheability,
        string? cacheControl,
        MissCapture capture,
        bool record)
    {
        // Size gate for HTML processing.
        if (buffer.Length > opts.MaxResponseBufferBytes ||
            !RequestClassifier.IsHtmlResponse(context.Response))
        {
            buffer.Seek(0, SeekOrigin.Begin);
            await buffer.CopyToAsync(originalBody, context.RequestAborted);
            return;
        }

        try
        {
            var underlying = buffer.GetBuffer();
            var htmlSpan = new ReadOnlySpan<byte>(
                underlying, 0, (int)buffer.Length);

            context.RequestAborted.ThrowIfCancellationRequested();
            using var result = _htmlProcessor.Process(
                htmlSpan, url, hostname, scheme);
            context.RequestAborted.ThrowIfCancellationRequested();

            if (result.Modified)
            {
                var linksBefore = context.Response.Headers.Link.ToArray();
                AddPreloadHeaders(context, result);
                capture.OwnLinks.AddRange(SubtractLines(
                    context.Response.Headers.Link.ToArray(), linksBefore));
                context.Response.Headers["X-PageSpeed"] = "MISS";
                SetVaryHeaders(context, PageSpeedContentType.Html, capture);

                var output = result.OutputMemory;
                context.Response.ContentLength = output.Length;
                await originalBody.WriteAsync(
                    output, context.RequestAborted);

                if (!record) return;

                var cacheable = TryWriteToCache(
                    url, hostname, scheme, output,
                    PageSpeedContentType.Html,
                    context.Response.ContentType,
                    result.NeedsRevalidation,
                    capture,
                    cacheControl);

                if (cacheable)
                    TryWriteEarlyHints(
                        url, hostname, scheme, result.EarlyHintsMemory);

                // [Critical #2] Skip worker notification if no-transform.
                if (cacheable && cacheability != ResponseCacheability.NoTransform &&
                    !WithholdHtmlNotify(PageSpeedContentType.Html, hostname))
                {
                    if (!_notifier.TryNotify(
                        url, hostname, scheme, PageSpeedContentType.Html, mask,
                        ShouldRequestAgentRender(context, opts),
                        hostVouched: HostVouched(hostname)))
                        _logger.LogDebug("TryNotify failed for HTML: {Url}", url);
                }
            }
            else
            {
                buffer.Seek(0, SeekOrigin.Begin);
                context.Response.ContentLength = buffer.Length;
                await buffer.CopyToAsync(
                    originalBody, context.RequestAborted);
            }
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            _logger.LogWarning(ex,
                "PageSpeed HTML processing failed, passing through original");

            if (!context.Response.HasStarted)
            {
                buffer.Seek(0, SeekOrigin.Begin);
                context.Response.ContentLength = buffer.Length;
                await buffer.CopyToAsync(
                    originalBody, context.RequestAborted);
            }
            else
            {
                _logger.LogError(ex,
                    "Response already started, cannot fall back");
            }
        }
    }

    /// <summary>
    /// Whether AllowedHosts names <paramref name="hostname"/>; see
    /// <see cref="AgentHostVouch"/>.
    /// </summary>
    private bool HostVouched(string hostname) =>
        AgentHostVouch.Vouches(_hostFiltering?.CurrentValue.AllowedHosts, hostname);

    /// <summary>
    /// While the worker's agent optimization is on, an HTML notification can
    /// make it render the page and fetch from the host name. Withheld for a
    /// host AllowedHosts does not name; the response itself is served as
    /// usual. An unreadable worker setting counts as on.
    /// </summary>
    private bool WithholdHtmlNotify(PageSpeedContentType type, string hostname)
    {
        if (type != PageSpeedContentType.Html || HostVouched(hostname)) return false;
        try
        {
            return _cache.ReadSharedConfigAgentEntitled() != 0;
        }
        catch
        {
            return true;
        }
    }

    /// <summary>
    /// Handles a cache MISS for images, CSS, and JS: caches the
    /// original response and notifies the worker for optimization
    /// (image transcoding, CSS/JS minification, compression).
    /// </summary>
    private async Task HandleAssetMiss(
        HttpContext context, Stream originalBody,
        RecyclableMemoryStream buffer, PageSpeedOptions opts,
        string url, string hostname, string scheme, uint mask,
        PageSpeedContentType psContentType,
        string? responseContentType,
        ResponseCacheability cacheability,
        string? cacheControl,
        MissCapture capture)
    {
        // Pass through the original response to the client.
        buffer.Seek(0, SeekOrigin.Begin);
        context.Response.ContentLength = buffer.Length;
        context.Response.Headers["X-PageSpeed"] = "MISS";
        SetVaryHeaders(context, psContentType, capture);
        await buffer.CopyToAsync(originalBody, context.RequestAborted);

        // [High #4] Size gate: skip caching for oversized assets.
        if (buffer.Length > opts.MaxAssetCacheBytes)
        {
            _logger.LogDebug(
                "Asset {Url} exceeds MaxAssetCacheBytes ({Size} > {Max}), skipping cache",
                url, buffer.Length, opts.MaxAssetCacheBytes);
            return;
        }

        // Cache the original (identity) content.
        var underlying = buffer.GetBuffer();
        var data = new ReadOnlyMemory<byte>(
            underlying, 0, (int)buffer.Length);

        var cacheable = TryWriteToCache(
            url, hostname, scheme, data,
            psContentType, responseContentType,
            needsRevalidation: false,
            capture: capture,
            cacheControl: context.Response.Headers.CacheControl.ToString());

        // [Critical #2] Skip worker notification if no-transform.
        if (cacheable && cacheability != ResponseCacheability.NoTransform)
        {
            if (!_notifier.TryNotify(url, hostname, scheme, psContentType, mask,
                                     hostVouched: HostVouched(hostname)))
                _logger.LogDebug("TryNotify failed for {Type}: {Url}", psContentType, url);
        }
    }

    /// <summary>
    /// Stores the response. Returns false when the response is not cached
    /// because its headers cannot be reproduced on a hit (or could not be
    /// stored); true otherwise, including when the entry write itself fails.
    /// </summary>
    private bool TryWriteToCache(
        string url, string hostname, string scheme,
        ReadOnlyMemory<byte> output,
        PageSpeedContentType contentType,
        string? responseContentType,
        bool needsRevalidation,
        MissCapture capture,
        string? cacheControl = null)
    {
        // The request-independent headers the application set are stored first
        // and replayed on every hit; the native gate decides which headers
        // those are. A response with a header it cannot reproduce is served
        // from the origin each time instead of from the cache.
        byte flags = 0;
        try
        {
            // The final headers can make the response uncacheable after the
            // fact (a cookie added when the response starts).
            if (capture.Uncacheable || capture.UncacheableNow)
            {
                _logger.LogDebug(
                    "Response for {Url} is not cacheable; not caching", url);
                return false;
            }
            var originHeaders = capture.Headers;
            var sidecar = _cache.WriteHeadersSidecar(
                url, hostname, scheme, originHeaders);
            if (sidecar.Verdict != HeadersSidecarVerdict.SwapEligible)
            {
                _logger.LogDebug(
                    "Response for {Url} has headers that cannot be replayed from the cache; not caching",
                    url);
                return false;
            }
            // Callers record only responses to anonymous requests.
            flags |= CacheFlags.HeadersClassified | CacheFlags.AnonymousRecorded;
            if (sidecar.Stored) flags |= CacheFlags.HeadersSidecarRequired;
            if (VariesOnAccept(originHeaders)) flags |= CacheFlags.OriginVariesAccept;
        }
        catch (CacheNotOpenException)
        {
            ReportCacheNotOpen();
            return false;
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Storing response headers failed for {Url}; not caching", url);
            return false;
        }

        try
        {
            if (needsRevalidation) flags |= CacheFlags.NeedsRevalidation;

            // Parse origin Cache-Control to derive origin_cc_flags.
            var origin = ParseOriginCacheControl(cacheControl);

            var writeParams = new CacheWriteParams
            {
                AlternateId = (byte)DefaultMask,
                ContentLength = (ulong)output.Length,
                FullMask = DefaultMask,
                ContentType = contentType,
                Flags = flags,
                OriginContentType = responseContentType,
                OriginCcFlags = origin.Flags,
                OriginSMaxage = origin.SMaxage,
            };
            using var writer = _cache.BeginWrite(
                url, hostname, scheme, writeParams);
            writer.Write(output.Span);
            writer.Commit();
        }
        catch (CacheNotOpenException)
        {
            ReportCacheNotOpen();
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Cache write failed for {Url}", url);
        }
        return true;
    }

    /// <summary>
    /// A write skipped because the cache is not open (PageSpeed was enabled
    /// but the volume could not be opened; the open failure itself is logged
    /// as an error). Logged once per failure state, not per response.
    /// </summary>
    private void ReportCacheNotOpen()
    {
        if (_cache is DeferredPageSpeedCache deferred && !deferred.TryClaimNotOpenReport())
            return;
        _logger.LogWarning(
            "The PageSpeed cache is not open; responses are optimized but not "
            + "cached until it opens (see the earlier error)");
    }

    private void TryWriteEarlyHints(
        string url, string hostname, string scheme,
        ReadOnlyMemory<byte> earlyHints)
    {
        // An empty hint list must still overwrite the sentinel:
        // reprocessing can legitimately drop every hint (e.g. async-CSS
        // newly defers the only stylesheet once it caches), and a
        // surviving sentinel from an earlier pass would keep re-promoting
        // the demoted download forever. Readers treat empty content as
        // "no hints"; the managed cache API has no per-alternate remove.
        try
        {
            using var writer = _cache.BeginWriteSentinel(
                url, hostname, scheme, SentinelId.EarlyHints,
                (ulong)earlyHints.Length);
            writer.Write(earlyHints.Span);
            writer.Commit();
        }
        catch (CacheNotOpenException)
        {
            ReportCacheNotOpen();
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Early hints cache write failed for {Url}", url);
        }
    }

    private void AddLinkHeadersFromCache(
        HttpContext context, string url, string hostname, string scheme)
    {
        try
        {
            using var hints = _cache.ReadEarlyHints(url, hostname, scheme);
            if (hints == null) return;
            AddLinkHeadersFromHints(context, hints.ContentMemory);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex,
                "Early hints read failed for {Url}", url);
        }
    }

    /// <summary>
    /// Adds Link headers from cached early hints on the MISS path.
    /// These hints were generated during a prior HTML processing and
    /// provide preload/preconnect signals to the browser before the
    /// current response is fully generated.
    /// </summary>
    private void AddEarlyHintLinkHeaders(
        HttpContext context, string url, string hostname, string scheme,
        bool requestHasCookie)
    {
        try
        {
            using var hints = _cache.ReadEarlyHints(url, hostname, scheme);
            if (hints == null) return;
            // The hints were taken from the stored page, so a request
            // carrying a cookie gets them only when that page would be
            // served to it.
            if (requestHasCookie && !StoredPageSharedWithCookieRequest(
                    url, hostname, scheme))
                return;
            AddLinkHeadersFromHints(context, hints.ContentMemory);
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex,
                "Early hints pre-send failed for {Url}", url);
        }
    }

    /// <summary>
    /// Classifies a response Content-Type header into a PageSpeedContentType.
    /// Uses managed string matching (no P/Invoke) so tests work without
    /// the native library.
    /// </summary>
    internal static PageSpeedContentType ClassifyResponseContentType(
        string? contentType)
    {
        if (contentType == null) return PageSpeedContentType.Other;
        if (contentType.StartsWith("text/html", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Html;
        if (contentType.StartsWith("text/css", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Css;
        if (contentType.StartsWith("text/javascript", StringComparison.OrdinalIgnoreCase) ||
            contentType.StartsWith("application/javascript", StringComparison.OrdinalIgnoreCase) ||
            contentType.StartsWith("application/x-javascript", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Js;
        if (contentType.StartsWith("image/", StringComparison.OrdinalIgnoreCase))
            return PageSpeedContentType.Image;
        return PageSpeedContentType.Other;
    }

    /// <summary>
    /// The origin's Cache-Control as ps_parse_cache_control reports it: the
    /// PS_CC_ORIGIN_* flags, max-age and s-maxage. All zero without a value.
    /// </summary>
    private (ushort Flags, uint MaxAge, uint SMaxage) ParseOriginCacheControl(
        string? cacheControl)
    {
        if (string.IsNullOrEmpty(cacheControl)) return (0, 0, 0);
        var cc = new Native.NativeCacheControl
        {
            StructSize = (nuint)Marshal.SizeOf<Native.NativeCacheControl>()
        };

        // cacheControl is the response's joined Cache-Control value
        // (StringValues.ToString() -- multiple header lines join with
        // ", ", which is exactly the RFC 9111 single-list form the
        // parser walks, quoting included), so ONE call parses the list.
        int err = Native.NativePageSpeed.ParseCacheControl(cacheControl, ref cc);
        if (err != 0) // PS_OK
        {
            _logger.LogDebug(
                "ParseCacheControl failed for '{Value}': {Err}",
                cacheControl, err);
        }
        return (cc.CcFlags, cc.MaxAge, cc.SMaxage);
    }

    /// <summary>
    /// The cookie rule's record-side answer for this response
    /// (CookieRequestCacheRule.CookieRequestMayStore), from its Content-Type,
    /// Cache-Control and, without a Cache-Control line, Expires.
    /// </summary>
    private bool CookieRequestMayStore(HttpResponse response, string? cacheControl)
    {
        var (flags, maxAge, sMaxage) = ParseOriginCacheControl(cacheControl);
        CookieRequestCacheRule.FoldExpires(
            ref flags, ref maxAge,
            response.Headers.Expires.ToString(), response.Headers.Date.ToString(),
            DateTimeOffset.UtcNow);
        return CookieRequestCacheRule.CookieRequestMayStore(
            requestHasCookie: true,
            CookieRequestCacheRule.IsStaticAsset(response.ContentType),
            CookieRequestCacheRule.HtmlExplicitlyShared(flags, sMaxage),
            CookieRequestCacheRule.OriginStatesLifetime(flags, maxAge, sMaxage));
    }

    /// <summary>
    /// Classifies the response cacheability based on Cache-Control directives
    /// and Set-Cookie presence. Returns Uncacheable for no-store/private/Set-Cookie,
    /// NoTransform if the origin forbids transformation, Cacheable otherwise.
    /// </summary>
    internal static ResponseCacheability ClassifyCacheability(
        HttpResponse response)
    {
        // Set-Cookie means the response is personalized — never cache.
        if (response.Headers.SetCookie.Count > 0)
            return ResponseCacheability.Uncacheable;

        var cc = response.Headers.CacheControl.ToString();
        if (string.IsNullOrEmpty(cc))
            return ResponseCacheability.Cacheable;

        // Parse Cache-Control directives (case-insensitive).
        if (ContainsDirective(cc, "no-store") ||
            ContainsDirective(cc, "private"))
            return ResponseCacheability.Uncacheable;

        if (ContainsDirective(cc, "no-transform"))
            return ResponseCacheability.NoTransform;

        return ResponseCacheability.Cacheable;
    }

    /// <summary>
    /// Checks if a Cache-Control header value contains the given directive.
    /// Handles comma-separated directives with optional whitespace.
    /// </summary>
    internal static bool ContainsDirective(string cacheControl, string directive)
    {
        var span = cacheControl.AsSpan();
        while (span.Length > 0)
        {
            int comma = span.IndexOf(',');
            var token = comma >= 0 ? span[..comma] : span;
            token = token.Trim();

            // Directive may have a value (e.g. "max-age=3600").
            int eq = token.IndexOf('=');
            var name = eq >= 0 ? token[..eq].Trim() : token;

            if (name.Equals(directive, StringComparison.OrdinalIgnoreCase))
                return true;

            span = comma >= 0 ? span[(comma + 1)..] : [];
        }
        return false;
    }

    /// <summary>
    /// Weak ETag derived from the bytes of a cached body: the first 128 bits
    /// of its SHA-256, in lowercase hex.
    /// </summary>
    internal static string ComputeContentETag(ReadOnlySpan<byte> content)
    {
        Span<byte> hash = stackalloc byte[32];
        System.Security.Cryptography.SHA256.HashData(content, hash);
        return $"W/\"{Convert.ToHexString(hash[..16]).ToLowerInvariant()}\"";
    }

    /// <summary>
    /// Checks if an If-None-Match header value matches the given ETag.
    /// Handles comma-separated ETag lists per RFC 7232 §3.2.
    /// Supports wildcard "*" matching.
    /// </summary>
    internal static bool MatchesETag(string ifNoneMatch, string etag)
    {
        var span = ifNoneMatch.AsSpan().Trim();
        if (span.SequenceEqual("*"))
            return true;

        while (span.Length > 0)
        {
            int comma = span.IndexOf(',');
            var token = comma >= 0 ? span[..comma] : span;
            token = token.Trim();

            // Weak comparison (RFC 7232 §2.3.2): both W/ and strong
            // ETags match for conditional GET. Compare the full token.
            if (token.Equals(etag, StringComparison.Ordinal))
                return true;

            span = comma >= 0 ? span[(comma + 1)..] : [];
        }
        return false;
    }

    /// <summary>
    /// Detects browser force-refresh (Ctrl+F5). Browsers send
    /// Cache-Control: no-cache and/or Pragma: no-cache to indicate
    /// the user wants a fresh response from origin.
    /// </summary>
    internal static bool IsForceRefresh(HttpRequest request)
    {
        var cc = request.Headers.CacheControl.ToString();
        if (!string.IsNullOrEmpty(cc) &&
            ContainsDirective(cc, "no-cache"))
            return true;

        var pragma = request.Headers.Pragma.ToString();
        return !string.IsNullOrEmpty(pragma) &&
               ContainsDirective(pragma, "no-cache");
    }

    /// <summary>
    /// Checks if the request path matches any exclusion pattern.
    /// Patterns containing * or ? are matched as globs; others as prefixes
    /// for backward compatibility.
    /// </summary>
    // Serve the CSP-safe async-CSS loader JS (immutable, long-cache). GET ships
    // the body; HEAD ships headers only.
    private static async Task ServeAsyncCssLoaderAsync(HttpContext context)
    {
        var bytes = AsyncCssLoaderJsBytes.Value;
        var response = context.Response;
        response.StatusCode = StatusCodes.Status200OK;
        response.ContentType = "text/javascript; charset=utf-8";
        // The path is CONTENT-ADDRESSED (AsyncCssLoaderPath embeds a hash of the
        // loader body), so a future loader change yields a NEW path and can never
        // be masked by a stale cache — immutable+1yr is correct here, in lockstep
        // with the nginx module's Cache-Control for the same path.
        response.Headers.CacheControl = "public, max-age=31536000, immutable";
        response.Headers["X-Content-Type-Options"] = "nosniff";
        // Provenance marker so a deploy smoke can assert the loader was served
        // by the middleware (not a 200 from some other handler), matching nginx.
        response.Headers["X-PageSpeed"] = "async-css-loader";
        response.ContentLength = bytes.Length;
        if (!HttpMethods.IsHead(context.Request.Method))
        {
            await response.Body.WriteAsync(bytes);
        }
    }

    private static bool IsExcluded(
        HttpRequest request, PageSpeedOptions opts)
    {
        var path = request.Path.Value;
        if (path == null) return false;

        foreach (var pattern in opts.ExcludePaths)
        {
            if (pattern.AsSpan().IndexOfAny('*', '?') >= 0)
            {
                if (GlobMatch(path, pattern))
                    return true;
            }
            else
            {
                if (path.StartsWith(pattern,
                    StringComparison.OrdinalIgnoreCase))
                    return true;
            }
        }
        return false;
    }

    /// <summary>
    /// Simple glob matcher supporting * (any sequence) and ? (single char).
    /// Case-insensitive. Does not support character classes or braces.
    /// </summary>
    internal static bool GlobMatch(
        ReadOnlySpan<char> input, ReadOnlySpan<char> pattern)
    {
        int ip = 0, pp = 0;
        int starIp = -1, starPp = -1;

        while (ip < input.Length)
        {
            if (pp < pattern.Length &&
                (char.ToLowerInvariant(pattern[pp]) ==
                     char.ToLowerInvariant(input[ip]) ||
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

        while (pp < pattern.Length && pattern[pp] == '*')
            pp++;

        return pp == pattern.Length;
    }

    /// <summary>
    /// Adds Link preload/preconnect headers from an HtmlResult's early hints.
    /// </summary>
    private static void AddPreloadHeaders(
        HttpContext context, HtmlResult result)
    {
        var hints = result.EarlyHintsMemory;
        if (hints.IsEmpty) return;
        AddLinkHeadersFromHints(context, hints);
    }

    /// <summary>
    /// Shared hint-line parser: adds Link headers from newline-separated
    /// early hints data (used by both HIT and MISS paths).
    /// </summary>
    internal static void AddLinkHeadersFromHints(
        HttpContext context, ReadOnlyMemory<byte> hints)
    {
        var hintsSpan = hints.Span;
        int start = 0;
        for (int i = 0; i <= hintsSpan.Length; i++)
        {
            if (i == hintsSpan.Length || hintsSpan[i] == (byte)'\n')
            {
                if (i > start)
                {
                    var line = hintsSpan[start..i];
                    var value = BuildLinkHeader(line);
                    if (value != null)
                        context.Response.Headers.Append("Link", value);
                }
                start = i + 1;
            }
        }
    }

    private static string? BuildLinkHeader(ReadOnlySpan<byte> line)
    {
        ReadOnlySpan<byte> preconnectCors = "preconnect-cors:"u8;
        ReadOnlySpan<byte> preconnect = "preconnect:"u8;
        ReadOnlySpan<byte> image = "image:"u8;

        string url;
        string rel;

        // CORS-mode origin: crossorigin must ride along so the warmed
        // connection pool matches the request mode of the motivating
        // resource (fonts, crossorigin scripts, ES modules).
        if (line.StartsWith(preconnectCors))
        {
            url = Encoding.UTF8.GetString(line[preconnectCors.Length..]);
            rel = "preconnect; crossorigin";
        }
        else if (line.StartsWith(preconnect))
        {
            url = Encoding.UTF8.GetString(line[preconnect.Length..]);
            rel = "preconnect";
        }
        else if (line.StartsWith(image))
        {
            url = Encoding.UTF8.GetString(line[image.Length..]);
            rel = "preload; as=image";
        }
        else
        {
            url = Encoding.UTF8.GetString(line);
            rel = "preload; as=style";
        }

        if (url.AsSpan().IndexOfAny(UnsafeLinkChars) >= 0)
            return null;

        return $"<{url}>; rel={rel}";
    }
}

/// <summary>
/// Response cacheability classification based on Cache-Control directives.
/// </summary>
public enum ResponseCacheability
{
    /// <summary>Response can be cached and transformed.</summary>
    Cacheable,

    /// <summary>Response must not be cached (no-store, private, Set-Cookie).</summary>
    Uncacheable,

    /// <summary>Response can be cached but must not be transformed (no-transform).</summary>
    NoTransform,
}

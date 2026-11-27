// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Buffers.Binary;
using System.Net.Http;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.AspNetCore.HostFiltering;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NSubstitute;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class MiddlewareTests
{
    private readonly IHtmlProcessor _htmlProcessor;
    private readonly IPageSpeedCache _cache;
    private readonly WorkerNotificationService _notifier;
    private readonly IOptionsMonitor<PageSpeedOptions> _optionsMonitor;
    private readonly ILogger<PageSpeedMiddleware> _logger;
    private readonly PageSpeedOptions _options;

    public MiddlewareTests()
    {
        _htmlProcessor = Substitute.For<IHtmlProcessor>();
        _cache = new NullPageSpeedCache();
        _logger = Substitute.For<ILogger<PageSpeedMiddleware>>();
        _options = new PageSpeedOptions { Enabled = true };
        _optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        _optionsMonitor.CurrentValue.Returns(_options);

        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        _notifier = new WorkerNotificationService(
            notifierOptions,
            new WeAmp.PageSpeed.AspNetCore.Internal.InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());
    }

    private PageSpeedMiddleware CreateMiddleware(RequestDelegate next) =>
        new(next, _optionsMonitor, _htmlProcessor, _cache, _notifier, _logger);

    /// <summary>
    /// Asserts that IHtmlProcessor.Process was never called.
    /// We use ReceivedCalls() because NSubstitute's Arg.Any does not
    /// support ReadOnlySpan (ref struct).
    /// </summary>
    private void AssertProcessNotCalled()
    {
        var calls = _htmlProcessor.ReceivedCalls()
            .Where(c => c.GetMethodInfo().Name == "Process");
        Assert.Empty(calls);
    }

    /// <summary>
    /// Captures OnStarting callbacks (DefaultHttpContext's stock response
    /// feature discards them) so a header registered via OnStarting is
    /// observable after the pipeline runs.
    /// </summary>
    private sealed class StartingAwareResponseFeature : HttpResponseFeature
    {
        private readonly List<(Func<object, Task> Callback, object State)> _callbacks = new();

        public override void OnStarting(Func<object, Task> callback, object state) =>
            _callbacks.Add((callback, state));

        public async Task FireOnStartingAsync()
        {
            foreach (var (callback, state) in _callbacks)
                await callback(state);
        }
    }

    [Fact]
    public async Task ProcessedHtmlResponse_CarriesNoWarnHeader()
    {
        // The middleware has no notion of a licensing state any more: an HTML
        // response that runs the whole pipeline must not register or emit any
        // x-pagespeed-warn header, on the direct path or via OnStarting. (The
        // unconfigured processor substitute makes the transform fall back to
        // the original body, which is still a complete 200 HTML pass.)
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            var body = Encoding.UTF8.GetBytes("<html>original</html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var responseFeature = new StartingAwareResponseFeature();
        var context = new DefaultHttpContext();
        context.Features.Set<IHttpResponseFeature>(responseFeature);
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Response.Body = new MemoryStream();

        await middleware.InvokeAsync(context);
        await responseFeature.FireOnStartingAsync();

        Assert.Equal(200, context.Response.StatusCode);
        Assert.False(context.Response.Headers.ContainsKey("x-pagespeed-warn"),
            "no licensing state exists; the warn header must never be emitted");
        Assert.DoesNotContain(context.Response.Headers.Keys,
            k => k.Contains("warn", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task ExcludedPath_PassesThrough()
    {
        // Arrange: /api/ is in the default ExcludePaths list.
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/api/test";
        context.Request.Headers.Accept = "text/html";

        // Act
        await middleware.InvokeAsync(context);

        // Assert
        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task NonHtmlAcceptHeader_PassesThrough()
    {
        // Arrange: Accept header that does not include text/html or */*.
        // Middleware no longer short-circuits on Accept — it processes
        // all content types. But downstream returns empty body, so no
        // HTML processing occurs.
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            ctx.Response.StatusCode = 200;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/page";
        context.Request.Headers.Accept = "application/json";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert
        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task Non2xxResponse_PassesThrough()
    {
        // Arrange: downstream returns 404.
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 404;
            ctx.Response.ContentType = "text/html";
            var body = Encoding.UTF8.GetBytes("<html>Not Found</html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/missing";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: response passes through without HTML processing.
        AssertProcessNotCalled();

        responseBody.Seek(0, SeekOrigin.Begin);
        var output = Encoding.UTF8.GetString(responseBody.ToArray());
        Assert.Contains("Not Found", output);
    }

    [Fact]
    public async Task NonHtmlContentType_PassesThrough()
    {
        // Arrange: downstream returns JSON content type.
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/json";
            var body = Encoding.UTF8.GetBytes("{\"ok\":true}");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/data";
        context.Request.Headers.Accept = "text/html,application/json";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: response passes through without HTML processing.
        AssertProcessNotCalled();

        responseBody.Seek(0, SeekOrigin.Begin);
        var output = Encoding.UTF8.GetString(responseBody.ToArray());
        Assert.Contains("ok", output);
    }

    [Fact]
    public async Task WebSocketUpgradeRequest_PassesThrough()
    {
        // Arrange: simulate a WebSocket upgrade request.
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/ws";
        context.Request.Headers.Accept = "text/html";
        // Set IHttpWebSocketFeature to simulate a WebSocket upgrade.
        context.Features.Set<IHttpWebSocketFeature>(
            new FakeWebSocketFeature());

        // Act
        await middleware.InvokeAsync(context);

        // Assert
        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task MiddlewareDisabled_PassesThrough()
    {
        // Arrange: disable the middleware.
        _options.Enabled = false;

        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/page";
        context.Request.Headers.Accept = "text/html";

        // Act
        await middleware.InvokeAsync(context);

        // Assert
        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task WildcardAcceptHeader_IsProcessed()
    {
        // Arrange: Accept: */* should be treated as HTML-compatible.
        // Downstream returns a 200 HTML response. The middleware will
        // attempt to call Process, but with our NSubstitute mock
        // returning null, we just verify the request reaches downstream.
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            var body = Encoding.UTF8.GetBytes("<html></html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/page";
        context.Request.Headers.Accept = "*/*";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: next was called (request was not short-circuited).
        Assert.True(nextCalled);
    }

    [Fact]
    public async Task NoAcceptHeader_IsProcessed()
    {
        // Arrange: no Accept header should be treated as accepting anything.
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            var body = Encoding.UTF8.GetBytes("<html></html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/page";
        // No Accept header set.
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: next was called (request was not short-circuited).
        Assert.True(nextCalled);
    }

    [Theory]
    [InlineData("/signalr/hub")]
    [InlineData("/_blazor/negotiate")]
    [InlineData("/_framework/blazor.boot.json")]
    public async Task DefaultExcludedPaths_PassThrough(string path)
    {
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = path;
        context.Request.Headers.Accept = "text/html";

        await middleware.InvokeAsync(context);

        Assert.True(nextCalled);
    }

    [Fact]
    public async Task EmptyResponseBody_PassesThrough()
    {
        // Arrange: downstream returns 200 HTML but with empty body.
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            // No body written.
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/empty";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: empty body, no processing.
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task NextThrows_RestoresBodyAndRethrows()
    {
        // Arrange: _next throws an exception after writing to the buffer.
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            throw new InvalidOperationException("downstream failure");
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/page";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act & Assert: the exception propagates and the body is restored.
        await Assert.ThrowsAsync<InvalidOperationException>(
            () => middleware.InvokeAsync(context));

        // Body stream should be restored to the original.
        Assert.Same(responseBody, context.Response.Body);
    }

    [Theory]
    [InlineData("no-store", ResponseCacheability.Uncacheable)]
    [InlineData("private", ResponseCacheability.Uncacheable)]
    [InlineData("no-store, max-age=0", ResponseCacheability.Uncacheable)]
    [InlineData("private, no-cache", ResponseCacheability.Uncacheable)]
    [InlineData("no-transform", ResponseCacheability.NoTransform)]
    [InlineData("public, no-transform, max-age=3600", ResponseCacheability.NoTransform)]
    [InlineData("public, max-age=3600", ResponseCacheability.Cacheable)]
    [InlineData("max-age=0, must-revalidate", ResponseCacheability.Cacheable)]
    [InlineData("", ResponseCacheability.Cacheable)]
    public void ClassifyCacheability_ParsesCacheControl(
        string cacheControl, ResponseCacheability expected)
    {
        var context = new DefaultHttpContext();
        if (!string.IsNullOrEmpty(cacheControl))
            context.Response.Headers.CacheControl = cacheControl;

        var result = PageSpeedMiddleware.ClassifyCacheability(
            context.Response);

        Assert.Equal(expected, result);
    }

    [Fact]
    public void ClassifyCacheability_SetCookie_IsUncacheable()
    {
        var context = new DefaultHttpContext();
        context.Response.Headers.SetCookie = "session=abc123";

        var result = PageSpeedMiddleware.ClassifyCacheability(
            context.Response);

        Assert.Equal(ResponseCacheability.Uncacheable, result);
    }

    [Fact]
    public async Task NoStoreResponse_PassesThroughWithoutCaching()
    {
        // Arrange: downstream returns 200 HTML with Cache-Control: no-store.
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            ctx.Response.Headers.CacheControl = "no-store";
            var body = Encoding.UTF8.GetBytes("<html>private</html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/private";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: response passes through without HTML processing.
        AssertProcessNotCalled();

        responseBody.Seek(0, SeekOrigin.Begin);
        var output = Encoding.UTF8.GetString(responseBody.ToArray());
        Assert.Contains("private", output);
    }

    [Fact]
    public async Task SetCookieResponse_PassesThroughWithoutCaching()
    {
        // Arrange: downstream returns HTML with Set-Cookie.
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            ctx.Response.Headers.SetCookie = "session=abc";
            var body = Encoding.UTF8.GetBytes("<html>personalized</html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/login";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: no HTML processing or caching.
        AssertProcessNotCalled();

        responseBody.Seek(0, SeekOrigin.Begin);
        var output = Encoding.UTF8.GetString(responseBody.ToArray());
        Assert.Contains("personalized", output);
    }

    [Theory]
    [InlineData("text/html", PageSpeedContentType.Html)]
    [InlineData("text/html; charset=utf-8", PageSpeedContentType.Html)]
    [InlineData("text/css", PageSpeedContentType.Css)]
    [InlineData("text/css; charset=utf-8", PageSpeedContentType.Css)]
    [InlineData("text/javascript", PageSpeedContentType.Js)]
    [InlineData("application/javascript", PageSpeedContentType.Js)]
    [InlineData("image/jpeg", PageSpeedContentType.Image)]
    [InlineData("image/png", PageSpeedContentType.Image)]
    [InlineData("image/webp", PageSpeedContentType.Image)]
    [InlineData("application/json", PageSpeedContentType.Other)]
    [InlineData("application/pdf", PageSpeedContentType.Other)]
    [InlineData(null, PageSpeedContentType.Other)]
    public void ClassifyResponseContentType_ClassifiesCorrectly(
        string? contentType, PageSpeedContentType expected)
    {
        var result = PageSpeedMiddleware.ClassifyResponseContentType(
            contentType);
        Assert.Equal(expected, result);
    }

    [Fact]
    public async Task PostRequest_PassesThroughWithoutCaching()
    {
        // Arrange: POST requests must not be cached.
        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/html";
            var body = Encoding.UTF8.GetBytes("<html>form result</html>");
            return ctx.Response.Body.WriteAsync(body, 0, body.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Method = "POST";
        context.Request.Path = "/submit";
        context.Request.Headers.Accept = "text/html";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: passes through without processing.
        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task ImageResponse_PassesThroughAndSetsVary()
    {
        // Arrange: downstream returns a JPEG image.
        var imageBytes = new byte[] { 0xFF, 0xD8, 0xFF, 0xE0 };
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "image/jpeg";
            return ctx.Response.Body.WriteAsync(imageBytes, 0, imageBytes.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/photo.jpg";
        context.Request.Headers.Accept = "image/webp,image/jpeg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: body passed through, correct headers set.
        Assert.Equal(imageBytes.Length, responseBody.Length);
        Assert.Equal("MISS", context.Response.Headers["X-PageSpeed"].ToString());
        Assert.Equal(
            "Accept, Save-Data, User-Agent",
            context.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task CssResponse_PassesThroughAndSetsVary()
    {
        // Arrange: downstream returns CSS.
        var cssBytes = Encoding.UTF8.GetBytes("body { color: red; }");
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "text/css; charset=utf-8";
            return ctx.Response.Body.WriteAsync(cssBytes, 0, cssBytes.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert
        Assert.Equal(cssBytes.Length, responseBody.Length);
        Assert.Equal("MISS", context.Response.Headers["X-PageSpeed"].ToString());
        Assert.Equal(
            "Accept-Encoding",
            context.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task JsResponse_PassesThroughAndSetsVary()
    {
        // Arrange: downstream returns JavaScript.
        var jsBytes = Encoding.UTF8.GetBytes("console.log('hello');");
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "application/javascript";
            return ctx.Response.Body.WriteAsync(jsBytes, 0, jsBytes.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/js/app.js";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert
        Assert.Equal(jsBytes.Length, responseBody.Length);
        Assert.Equal("MISS", context.Response.Headers["X-PageSpeed"].ToString());
        Assert.Equal(
            "Accept-Encoding",
            context.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task NoTransformResponse_CachesButSkipsWorkerNotification()
    {
        // Arrange: downstream returns image with no-transform.
        // Since we can't easily verify notification wasn't sent (internal
        // service with no listener), we verify the response passes through
        // correctly with the no-transform directive.
        var imageBytes = new byte[] { 0xFF, 0xD8, 0xFF, 0xE0 };
        var middleware = CreateMiddleware(ctx =>
        {
            ctx.Response.StatusCode = 200;
            ctx.Response.ContentType = "image/jpeg";
            ctx.Response.Headers.CacheControl = "public, no-transform";
            return ctx.Response.Body.WriteAsync(imageBytes, 0, imageBytes.Length);
        });

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/photo.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");

        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: response passes through (cached, but worker not notified).
        Assert.Equal(imageBytes.Length, responseBody.Length);
        Assert.Equal("MISS", context.Response.Headers["X-PageSpeed"].ToString());
    }

    [Theory]
    [InlineData(
        "https://cdn.example.com/style.css",
        "<https://cdn.example.com/style.css>; rel=preload; as=style")]
    [InlineData(
        "image:/img/hero.jpg",
        "</img/hero.jpg>; rel=preload; as=image")]
    [InlineData(
        "preconnect:https://cdn.example.com",
        "<https://cdn.example.com>; rel=preconnect")]
    [InlineData(
        "preconnect-cors:https://fonts.gstatic.com",
        "<https://fonts.gstatic.com>; rel=preconnect; crossorigin")]
    public void AddLinkHeadersFromHints_ValidUrls(
        string hint, string expected)
    {
        var context = new DefaultHttpContext();
        var hints = Encoding.UTF8.GetBytes(hint);
        PageSpeedMiddleware.AddLinkHeadersFromHints(context, hints);
        Assert.Equal(expected,
            context.Response.Headers.Link.ToString());
    }

    [Theory]
    [InlineData("<script>alert('xss')</script>")]
    [InlineData("https://evil.com\"; injection")]
    [InlineData("https://evil.com; extra")]
    [InlineData("https://evil.com, extra")]
    public void AddLinkHeadersFromHints_RejectsInjection(string hint)
    {
        var context = new DefaultHttpContext();
        var hints = Encoding.UTF8.GetBytes(hint);
        PageSpeedMiddleware.AddLinkHeadersFromHints(context, hints);
        Assert.Equal(0, context.Response.Headers.Link.Count);
    }

    [Fact]
    public void AddLinkHeadersFromHints_RejectsNullByte()
    {
        var context = new DefaultHttpContext();
        // Build hint bytes with embedded null byte manually.
        var hints = Encoding.UTF8.GetBytes("https://evil.com")
            .Concat(new byte[] { 0x00 })
            .Concat(Encoding.UTF8.GetBytes("payload"))
            .ToArray();
        PageSpeedMiddleware.AddLinkHeadersFromHints(context, hints);
        Assert.Equal(0, context.Response.Headers.Link.Count);
    }

    [Fact]
    public void AddLinkHeadersFromHints_RejectsCrlfInjection()
    {
        // \r in a URL is rejected by UnsafeLinkChars. When the hint
        // data contains \r\n, the line splitter separates on \n, leaving
        // \r at the end of the first line — which is correctly rejected.
        var context = new DefaultHttpContext();
        var hints = Encoding.UTF8.GetBytes(
            "https://evil.com\r");
        PageSpeedMiddleware.AddLinkHeadersFromHints(context, hints);
        Assert.Equal(0, context.Response.Headers.Link.Count);
    }

    [Fact]
    public void AddLinkHeadersFromHints_MultipleLines()
    {
        var context = new DefaultHttpContext();
        var hints = Encoding.UTF8.GetBytes(
            "/css/style.css\nimage:/img/hero.jpg\npreconnect:https://cdn.example.com");
        PageSpeedMiddleware.AddLinkHeadersFromHints(context, hints);
        var links = context.Response.Headers.Link.ToArray();
        Assert.Equal(3, links.Length);
        Assert.Equal("</css/style.css>; rel=preload; as=style", links[0]);
        Assert.Equal("</img/hero.jpg>; rel=preload; as=image", links[1]);
        Assert.Equal("<https://cdn.example.com>; rel=preconnect", links[2]);
    }

    [Fact]
    public void AddLinkHeadersFromHints_EmptyInput()
    {
        var context = new DefaultHttpContext();
        PageSpeedMiddleware.AddLinkHeadersFromHints(
            context, ReadOnlyMemory<byte>.Empty);
        Assert.Equal(0, context.Response.Headers.Link.Count);
    }

    // ── HIT path tests ─────────────────────────────────────────

    [Fact]
    public async Task CacheHit_ServesContentAndSetsHeaders()
    {
        var body = Encoding.UTF8.GetBytes("<html>cached</html>");
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = body,
            Mask = 0x08, // Desktop/Identity
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html; charset=utf-8",
            CacheInsertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 60,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        responseBody.Seek(0, SeekOrigin.Begin);
        Assert.Equal("<html>cached</html>",
            Encoding.UTF8.GetString(responseBody.ToArray()));
        Assert.Equal("HIT",
            context.Response.Headers["X-PageSpeed"].ToString());
        Assert.Equal("text/html; charset=utf-8",
            context.Response.ContentType);
        Assert.Equal("no-cache",
            context.Response.Headers.CacheControl.ToString());
        Assert.Equal("Accept-Encoding, User-Agent",
            context.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task CacheHit_AgeHeaderSetFromInsertedAt()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 120;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/jpeg",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/test.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var ageStr = context.Response.Headers["Age"].ToString();
        Assert.NotEmpty(ageStr);
        var age = int.Parse(ageStr);
        Assert.InRange(age, 115, 135); // ~120s, widened for slow CI
    }

    [Fact]
    public async Task CacheHit_NoAgeWhenInsertedAtIsZero()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = 0,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Empty(context.Response.Headers["Age"].ToString());
    }

    [Fact]
    public async Task CacheHit_FutureInsertedAt_NoAgeHeader()
    {
        var futureTs = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() + 60;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = futureTs,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Empty(context.Response.Headers["Age"].ToString());
    }

    [Fact]
    public async Task CacheHit_MaxUint32InsertedAt_NoAgeHeader()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = uint.MaxValue,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Empty(context.Response.Headers["Age"].ToString());
    }

    [Fact]
    public async Task CacheHit_IdentityEncoding_NoContentEncodingHeader()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("<html></html>"),
            Mask = 0x08, // Desktop/Identity — no encoding bits
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Empty(
            context.Response.Headers.ContentEncoding.ToString());
    }

    [Fact]
    public async Task CacheHit_WebPImageContentType()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x01, // FormatWebP
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/jpeg",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/test.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("image/webp", context.Response.ContentType);
    }

    [Fact]
    public async Task CacheHit_AvifImageContentType()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x02, // FormatAvif
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/jpeg",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/test.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("image/avif", context.Response.ContentType);
    }

    [Fact]
    public async Task CacheHit_SvgImageContentType()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x03, // FormatSvg
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/png",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/icon.png";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("image/svg+xml", context.Response.ContentType);
    }

    [Fact]
    public async Task CacheHit_GzipEncoding()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x48, // Desktop + Gzip
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("gzip",
            context.Response.Headers.ContentEncoding.ToString());
    }

    [Fact]
    public async Task CacheHit_BrotliEncoding()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x88, // Desktop + Brotli
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("br",
            context.Response.Headers.ContentEncoding.ToString());
    }

    [Fact]
    public async Task CacheHit_ImageVaryHeaders()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0xFF, 0xD8 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/jpeg",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/photo.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("Accept, Save-Data, User-Agent",
            context.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task CacheHit_CssVaryHeaders()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("Accept-Encoding",
            context.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task CacheHit_NeedsRevalidation_NotifiesWorker()
    {
        // This test verifies the code path runs without error.
        // Worker notification goes to a non-existent socket, which is
        // swallowed by TryNotify. We just verify no exception propagates.
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("<html></html>"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
            NeedsRevalidation = true,
            Flags = CacheFlags.NeedsRevalidation,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("HIT",
            context.Response.Headers["X-PageSpeed"].ToString());
    }

    // While the worker's agent optimization is on, an HTML notification is
    // sent only for a host AllowedHosts names; a list that allows any host,
    // or none, names no host.
    [Theory]
    [InlineData("site.test", "site.test", true)]
    [InlineData("a.site.test", "*.site.test", true)]
    [InlineData("other.test", "site.test", false)]
    [InlineData("other.test", "*", false)]
    [InlineData("other.test", null, false)]
    public async Task HtmlRevalidation_AgentOn_NotifiesOnlyForAllowedHosts(
        string host, string? allowedHost, bool expectNotified)
    {
        var pending = await RevalidateHtml(host, allowedHost, agentEntitled: 1);
        Assert.Equal(expectNotified ? 1 : 0, pending);
    }

    // With agent optimization off, AllowedHosts plays no part.
    [Fact]
    public async Task HtmlRevalidation_AgentOff_NotifiesForAnyHost()
    {
        var pending = await RevalidateHtml("other.test", "*", agentEntitled: 0);
        Assert.Equal(1, pending);
    }

    // The notification carries the host check in its flags byte (bit 0,
    // HOST_VOUCHED): the worker runs the agent render and the llms.txt build
    // only for a vouched notification, so a host AllowedHosts names turns
    // them on and any other host leaves them off.
    [Theory]
    [InlineData("site.test", "site.test", 1, 0x01)]
    [InlineData("a.site.test", "*.site.test", 1, 0x01)]
    [InlineData("site.test", "site.test", 0, 0x01)]
    [InlineData("other.test", "site.test", 0, 0x00)]
    [InlineData("other.test", "*", 0, 0x00)]
    public async Task HtmlRevalidation_NotificationCarriesTheHostCheck(
        string host, string allowedHost, int agentEntitled, int expectedFlags)
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return; // named pipes

        var frame = await RevalidateHtmlAndReadFrame(host, allowedHost, agentEntitled);
        Assert.Equal(6, frame[4]);                // wire version
        Assert.Equal(0, frame[^2]);               // no site
        Assert.Equal(expectedFlags, frame[^1]);   // flags
    }

    private async Task<byte[]> RevalidateHtmlAndReadFrame(
        string host, string allowedHost, int agentEntitled)
    {
        var sockPath = $"/tmp/psv-{Guid.NewGuid():N}".Substring(0, 24) + ".sock";
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var listener = new Socket(
            AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            listener.Bind(new UnixDomainSocketEndPoint(sockPath));
            listener.Listen(1);

            var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
            notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
            var endpoint = new WeAmp.PageSpeed.AspNetCore.Internal.InternalWorkerEndpoint();
            var notifier = new WorkerNotificationService(
                notifierOptions, endpoint,
                Substitute.For<ILogger<WorkerNotificationService>>());
            await notifier.StartAsync(cts.Token);
            endpoint.SetSocketPath(sockPath);
            var acceptTask = listener.AcceptAsync(cts.Token).AsTask();

            var cache = new HitPageSpeedCache(new FakeReadResult
            {
                ContentMemory = Encoding.UTF8.GetBytes("<html></html>"),
                Mask = 0x08,
                ContentType = PageSpeedContentType.Html,
                OriginContentType = "text/html",
                NeedsRevalidation = true,
                Flags = CacheFlags.NeedsRevalidation,
            })
            {
                AgentEntitled = agentEntitled,
            };
            var hostFiltering = Substitute.For<IOptionsMonitor<HostFilteringOptions>>();
            hostFiltering.CurrentValue.Returns(new HostFilteringOptions
            {
                AllowedHosts = new List<string> { allowedHost },
            });
            var middleware = new PageSpeedMiddleware(
                _ => Task.CompletedTask, _optionsMonitor,
                _htmlProcessor, cache, notifier, _logger, hostFiltering);

            var context = new DefaultHttpContext();
            context.Request.Method = "GET";
            context.Request.Path = "/page";
            context.Request.Scheme = "http";
            context.Request.Host = new HostString(host);
            context.Response.Body = new MemoryStream();
            await middleware.InvokeAsync(context);
            Assert.Equal("HIT", context.Response.Headers["X-PageSpeed"].ToString());

            using var conn = await acceptTask;
            var header = await ReadExactlyAsync(conn, 4, cts.Token);
            var payload = await ReadExactlyAsync(
                conn, BinaryPrimitives.ReadInt32BigEndian(header), cts.Token);
            await notifier.StopAsync(cts.Token);
            return header.Concat(payload).ToArray();
        }
        finally
        {
            try { File.Delete(sockPath); } catch { /* best effort */ }
        }
    }

    private static async Task<byte[]> ReadExactlyAsync(
        Socket s, int n, CancellationToken ct)
    {
        var buf = new byte[n];
        int got = 0;
        while (got < n)
        {
            int r = await s.ReceiveAsync(buf.AsMemory(got), SocketFlags.None, ct);
            if (r == 0) throw new IOException("socket closed early");
            got += r;
        }
        return buf;
    }

    private async Task<int> RevalidateHtml(
        string host, string? allowedHost, int agentEntitled)
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("<html></html>"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
            NeedsRevalidation = true,
            Flags = CacheFlags.NeedsRevalidation,
        })
        {
            AgentEntitled = agentEntitled,
        };
        var hostFiltering = Substitute.For<IOptionsMonitor<HostFilteringOptions>>();
        hostFiltering.CurrentValue.Returns(new HostFilteringOptions
        {
            AllowedHosts = allowedHost is null
                ? new List<string>()
                : new List<string> { allowedHost },
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger, hostFiltering);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString(host);
        context.Response.Body = new MemoryStream();

        await middleware.InvokeAsync(context);

        Assert.Equal("HIT", context.Response.Headers["X-PageSpeed"].ToString());
        return _notifier.PendingCount;
    }

    [Fact]
    public async Task SafeMode_CssHit_DefaultMaxAge300()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("max-age=300", cc);
    }

    // ── CacheMode tests ────────────────────────────────────────

    [Fact]
    public async Task SafeMode_HtmlHit_GetsNoCache()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("<html>cached</html>"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("no-cache",
            context.Response.Headers.CacheControl.ToString());
    }

    [Fact]
    public async Task SafeMode_CssHit_IncludesMustRevalidate()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("must-revalidate", cc);
    }

    [Fact]
    public async Task SafeMode_ImageHit_DefaultMaxAge1800()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0xFF, 0xD8 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/jpeg",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/photo.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("max-age=1800", cc);
    }

    [Fact]
    public async Task SafeMode_CssHit_NoPublic()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.DoesNotContain("public", cc);
    }

    [Fact]
    public async Task AggressiveMode_CssHit_IncludesPublic()
    {
        _options.CacheMode = CacheMode.Aggressive;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("public", cc);
    }

    [Fact]
    public async Task AggressiveMode_CssHit_IncludesStaleIfError()
    {
        _options.CacheMode = CacheMode.Aggressive;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("stale-if-error=86400", cc);
    }

    [Fact]
    public async Task AggressiveMode_WorkerOptimizedHit_KeepsStaleIfError()
    {
        // #454: the worker-processed bit (kFlagWorkerProcessed, 0x02) is set on
        // EVERY worker-optimized variant. It used to be misread as the .NET
        // RevalidationRequired signal, so every aggressive-mode worker HIT wrongly
        // emitted must-revalidate instead of stale-if-error=86400, defeating
        // aggressive-mode resilience. After the Tier A fix the .NET side no longer
        // reads that bit as a revalidation signal, so a worker-optimized asset HIT
        // keeps stale-if-error=86400. The true origin-revalidation signal now lives
        // in C++ origin_cc_flags and is plumbed over the FFI via OriginCcFlags (#772).
        _options.CacheMode = CacheMode.Aggressive;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            // Worker-optimized variant: the worker-processed dedup bit is set.
            IsWorkerProcessed = true,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("stale-if-error=86400", cc);
        Assert.DoesNotContain("must-revalidate", cc);
        // public + max-age are still emitted in aggressive mode.
        Assert.Contains("public", cc);
        Assert.Contains("max-age", cc);
    }

    [Fact]
    public async Task AggressiveMode_CssHit_NoRevalidationFlag_KeepsStaleIfError()
    {
        // Companion regression guard: default (no revalidation_required
        // flag) must still emit stale-if-error so the #259 fix doesn't
        // overshoot into the normal aggressive-mode path.
        _options.CacheMode = CacheMode.Aggressive;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            // Flags = 0 (default) — no revalidation_required.
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("stale-if-error=86400", cc);
        Assert.DoesNotContain("must-revalidate", cc);
    }

    // The string-directive theory that used to live here tested the deleted
    // managed parser and encoded SHARED-cache expectations (proxy-revalidate
    // and s-maxage forcing revalidation). The private-cache semantics now live
    // in BuildAssetCacheControl_GatesStaleIfErrorOnRevalidationRequired below,
    // and the directive-to-flags mapping is the C parser's (ps_parse_cache_control,
    // covered by the library's own tests). (#772)

    [Theory]
    [InlineData(CacheMode.Safe, (ushort)0, "max-age=1800, must-revalidate")]
    [InlineData(CacheMode.Safe, CacheControlCcFlags.MustRevalidate, "max-age=1800, must-revalidate")]
    [InlineData(CacheMode.Aggressive, 0, "public, max-age=1800, stale-if-error=86400")]
    [InlineData(CacheMode.Aggressive, CacheControlCcFlags.MustRevalidate, "public, max-age=1800, must-revalidate")]
    [InlineData(CacheMode.Aggressive, CacheControlCcFlags.NoCache, "public, max-age=1800, must-revalidate")]
    [InlineData(CacheMode.Aggressive, CacheControlCcFlags.ProxyRevalidate, "public, max-age=1800, stale-if-error=86400")]
    [InlineData(CacheMode.Aggressive, CacheControlCcFlags.SMaxagePresent, "public, max-age=1800, stale-if-error=86400")]
    public void BuildAssetCacheControl_GatesStaleIfErrorOnRevalidationRequired(
        CacheMode mode, ushort originCcFlags, string expected)
    {
        // Direct unit test on the header-builder helper to lock the
        // RFC 9111 §4.2.4 / PR #252 contract at the formatting layer.
        Assert.Equal(expected,
            PageSpeedMiddleware.BuildAssetCacheControl(
                mode, 1800, originCcFlags));
    }

    [Fact]
    public async Task AggressiveMode_CssHit_NoMustRevalidate()
    {
        _options.CacheMode = CacheMode.Aggressive;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.DoesNotContain("must-revalidate", cc);
    }

    [Fact]
    public async Task AggressiveMode_HtmlHit_StillGetsNoCache()
    {
        _options.CacheMode = CacheMode.Aggressive;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("<html>cached</html>"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal("no-cache",
            context.Response.Headers.CacheControl.ToString());
    }

    [Fact]
    public async Task AggressiveMode_ImageHit_UsesImageMaxAge()
    {
        _options.CacheMode = CacheMode.Aggressive;
        _options.ImageMaxAgeSeconds = 7200;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0xFF, 0xD8 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Image,
            OriginContentType = "image/jpeg",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/img/photo.jpg";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("max-age=7200", cc);
    }

    [Fact]
    public void DefaultCacheMode_IsSafe()
    {
        var opts = new PageSpeedOptions();
        Assert.Equal(CacheMode.Safe, opts.CacheMode);
    }

    [Fact]
    public async Task ZeroMaxAge_ProducesValidHeader()
    {
        _options.CssMaxAgeSeconds = 0;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("body{}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var cc = context.Response.Headers.CacheControl.ToString();
        Assert.Contains("max-age=0", cc);
        Assert.Contains("must-revalidate", cc);
    }

    // ── Conditional 304 tests ───────────────────────────────────

    [Fact]
    public async Task CacheHit_ETagHeaderSet()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 60;
        var body = Encoding.UTF8.GetBytes("<html>cached</html>");
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = body,
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var etag = context.Response.Headers.ETag.ToString();
        Assert.StartsWith("W/\"", etag);
        Assert.EndsWith("\"", etag);
        Assert.Equal(200, context.Response.StatusCode);
    }

    [Fact]
    public async Task CacheHit_LastModifiedHeaderSet()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 120;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var lastModified = context.Response.Headers["Last-Modified"].ToString();
        Assert.NotEmpty(lastModified);
        Assert.True(DateTimeOffset.TryParse(lastModified, out _));
    }

    [Fact]
    public async Task CacheHit_IfNoneMatchMatches_Returns304()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 60;
        var body = Encoding.UTF8.GetBytes("<html>cached</html>");
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = body,
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
            CacheInsertedAt = insertedAt,
        });

        // Generate the expected ETag.
        var expectedEtag = PageSpeedMiddleware.ComputeContentETag(body);

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfNoneMatch = expectedEtag;
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal(304, context.Response.StatusCode);
        Assert.Null(context.Response.ContentLength);
        Assert.Equal(0, responseBody.Length); // No body written.
    }

    private async Task<DefaultHttpContext> ServeHitAsync(
        byte[] body, uint insertedAt, string? ifNoneMatch = null)
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = body,
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });
        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);
        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        if (ifNoneMatch != null)
            context.Request.Headers.IfNoneMatch = ifNoneMatch;
        context.Response.Body = new MemoryStream();
        await middleware.InvokeAsync(context);
        return context;
    }

    [Fact]
    public async Task CacheHit_SameLengthPayloadsInSameSecond_GetDifferentETags()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 30;
        var a = await ServeHitAsync(Encoding.UTF8.GetBytes("a{color:red}"), insertedAt);
        var b = await ServeHitAsync(Encoding.UTF8.GetBytes("a{color:tan}"), insertedAt);
        var etagA = a.Response.Headers.ETag.ToString();
        var etagB = b.Response.Headers.ETag.ToString();
        Assert.Equal("a{color:red}".Length, "a{color:tan}".Length);
        Assert.NotEqual(etagA, etagB);
    }

    [Fact]
    public async Task CacheHit_IdenticalContent_KeepsETagAndRevalidates304()
    {
        var body = Encoding.UTF8.GetBytes("a{color:red}");
        var first = await ServeHitAsync(body, (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 90);
        var etag = first.Response.Headers.ETag.ToString();
        Assert.Equal(200, first.Response.StatusCode);

        // Re-inserted later with the same bytes: same tag, validator matches.
        var again = await ServeHitAsync(
            body, (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 5, etag);
        Assert.Equal(etag, again.Response.Headers.ETag.ToString());
        Assert.Equal(304, again.Response.StatusCode);

        // A different payload does not match the old validator.
        var changed = await ServeHitAsync(
            Encoding.UTF8.GetBytes("a{color:tan}"),
            (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 5, etag);
        Assert.Equal(200, changed.Response.StatusCode);
    }

    [Fact]
    public async Task CacheHit_NonMatchingIfNoneMatch_IgnoresIfModifiedSince()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 30;
        var first = await ServeHitAsync(Encoding.UTF8.GetBytes("a{color:red}"), insertedAt);
        var etagA = first.Response.Headers.ETag.ToString();
        var lastModifiedA = first.Response.Headers["Last-Modified"].ToString();

        // Body B replaces body A within the same second; the client
        // revalidates with A's tag and A's Last-Modified.
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("a{color:tan}"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });
        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);
        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfNoneMatch = etagA;
        context.Request.Headers.IfModifiedSince = lastModifiedA;
        context.Response.Body = new MemoryStream();
        await middleware.InvokeAsync(context);

        Assert.Equal(200, context.Response.StatusCode);
    }

    [Fact]
    public async Task CacheHit_IfNoneMatchStar_Returns304()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 10,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfNoneMatch = "*";
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal(304, context.Response.StatusCode);
    }

    [Fact]
    public async Task CacheHit_IfNoneMatchMismatch_Returns200()
    {
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 10,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfNoneMatch = "W/\"wrong-etag\"";
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal(200, context.Response.StatusCode);
        Assert.True(responseBody.Length > 0);
    }

    [Fact]
    public async Task CacheHit_IfModifiedSinceOld_Returns200()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 60;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        // If-Modified-Since is BEFORE the cache insertion time.
        var oldDate = DateTimeOffset.FromUnixTimeSeconds(insertedAt - 120);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfModifiedSince = oldDate.ToString("R");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal(200, context.Response.StatusCode);
    }

    [Fact]
    public async Task CacheHit_IfModifiedSinceRecent_Returns304()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 60;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        // If-Modified-Since is AFTER the cache insertion time.
        var recentDate = DateTimeOffset.FromUnixTimeSeconds(insertedAt + 10);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfModifiedSince = recentDate.ToString("R");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal(304, context.Response.StatusCode);
        Assert.Null(context.Response.ContentLength);
    }

    // ── ETag matching tests (RFC 7232 §3.2) ─────────────────────

    [Theory]
    [InlineData("W/\"abc\"", "W/\"abc\"", true)]
    [InlineData("W/\"abc\", W/\"def\"", "W/\"def\"", true)]
    [InlineData("W/\"abc\" , W/\"def\" , W/\"ghi\"", "W/\"def\"", true)]
    [InlineData("W/\"abc\"", "W/\"xyz\"", false)]
    [InlineData("*", "W/\"anything\"", true)]
    [InlineData("W/\"abc-substring\"", "W/\"abc\"", false)]
    [InlineData("W/\"abc\"", "W/\"abc-substring\"", false)]
    public void MatchesETag_Patterns(
        string ifNoneMatch, string etag, bool expected)
    {
        Assert.Equal(expected,
            PageSpeedMiddleware.MatchesETag(ifNoneMatch, etag));
    }

    [Fact]
    public async Task CacheHit_IfNoneMatchMultiValue_Returns304()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 10;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        // Compute the expected ETag.
        var etag = PageSpeedMiddleware.ComputeContentETag(new byte[] { 0x01 });

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        // Send multiple ETags in a comma-separated list.
        context.Request.Headers.IfNoneMatch = $"W/\"wrong\", {etag}, W/\"other\"";
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.Equal(304, context.Response.StatusCode);
    }

    [Fact]
    public async Task CacheHit_IfNoneMatchSubstring_Returns200()
    {
        var insertedAt = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 10;
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = new byte[] { 0x01 },
            Mask = 0x08,
            ContentType = PageSpeedContentType.Css,
            OriginContentType = "text/css",
            CacheInsertedAt = insertedAt,
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, _optionsMonitor,
            _htmlProcessor, cache, _notifier, _logger);

        // Compute the expected ETag, then embed it as a substring of another.
        var etag = PageSpeedMiddleware.ComputeContentETag(new byte[] { 0x01 });
        var fakeEtag = $"W/\"prefix-{etag.Substring(3, etag.Length - 4)}-suffix\"";

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/css/style.css";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.IfNoneMatch = fakeEtag;
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        // Must NOT match — the old substring-based check would have
        // incorrectly returned 304 here.
        Assert.Equal(200, context.Response.StatusCode);
    }

    // ── Force-refresh bypass tests ──────────────────────────────

    [Theory]
    [InlineData("no-cache", true)]
    [InlineData("max-age=0, no-cache", true)]
    [InlineData("no-store", false)]
    [InlineData("max-age=0", false)]
    [InlineData("", false)]
    public void IsForceRefresh_CacheControl(string cc, bool expected)
    {
        var context = new DefaultHttpContext();
        if (!string.IsNullOrEmpty(cc))
            context.Request.Headers.CacheControl = cc;

        Assert.Equal(expected,
            PageSpeedMiddleware.IsForceRefresh(context.Request));
    }

    [Fact]
    public void IsForceRefresh_Pragma()
    {
        var context = new DefaultHttpContext();
        context.Request.Headers.Pragma = "no-cache";

        Assert.True(
            PageSpeedMiddleware.IsForceRefresh(context.Request));
    }

    [Fact]
    public async Task ForceRefresh_BypassesCacheHit()
    {
        // Cache returns a HIT, but force-refresh should bypass it.
        var cache = new HitPageSpeedCache(new FakeReadResult
        {
            ContentMemory = Encoding.UTF8.GetBytes("<html>cached</html>"),
            Mask = 0x08,
            ContentType = PageSpeedContentType.Html,
            OriginContentType = "text/html",
        });

        bool nextCalled = false;
        var middleware = new PageSpeedMiddleware(
            ctx =>
            {
                nextCalled = true;
                ctx.Response.StatusCode = 200;
                ctx.Response.ContentType = "text/html";
                var body = Encoding.UTF8.GetBytes("<html>fresh</html>");
                return ctx.Response.Body.WriteAsync(body, 0, body.Length);
            },
            _optionsMonitor, _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.CacheControl = "no-cache";
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        Assert.True(nextCalled, "Force-refresh should call origin");
    }

    // ── Glob path exclusion tests ───────────────────────────────

    [Theory]
    [InlineData("/img/photo.jpg", "/img/*.jpg", true)]
    [InlineData("/img/photo.png", "/img/*.jpg", false)]
    [InlineData("/assets/v1/app.js", "/assets/*/app.js", true)]
    [InlineData("/a/b/c", "/a/?/c", true)]
    [InlineData("/a/bc/c", "/a/?/c", false)]
    [InlineData("/hello", "*", true)]
    [InlineData("/foo/bar", "/foo/bar", true)]
    [InlineData("/FOO/BAR", "/foo/bar", true)] // case insensitive
    [InlineData("/foo", "/foobar", false)]
    public void GlobMatch_Patterns(string input, string pattern, bool expected)
    {
        Assert.Equal(expected,
            PageSpeedMiddleware.GlobMatch(input, pattern));
    }

    [Fact]
    public async Task GlobExcludePath_MatchesAndExcludes()
    {
        _options.ExcludePaths = ["/api/", "*.json"];

        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/data/config.json";
        context.Request.Headers.Accept = "text/html";

        await middleware.InvokeAsync(context);

        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    [Fact]
    public async Task GlobExcludePath_NoWildcard_PrefixMatch()
    {
        _options.ExcludePaths = ["/api/"];

        bool nextCalled = false;
        var middleware = CreateMiddleware(ctx =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });

        var context = new DefaultHttpContext();
        context.Request.Path = "/api/users";

        await middleware.InvokeAsync(context);

        Assert.True(nextCalled);
        AssertProcessNotCalled();
    }

    // ── Early hints on MISS path tests ──────────────────────────

    [Fact]
    public async Task MissPath_WithCachedEarlyHints_AddsLinkHeaders()
    {
        var hintsData = Encoding.UTF8.GetBytes(
            "/css/style.css\nimage:/img/hero.jpg");
        var cache = new EarlyHintsPageSpeedCache(hintsData);

        var middleware = new PageSpeedMiddleware(
            ctx =>
            {
                ctx.Response.StatusCode = 200;
                ctx.Response.ContentType = "text/html";
                var body = Encoding.UTF8.GetBytes("<html>fresh</html>");
                return ctx.Response.Body.WriteAsync(body, 0, body.Length);
            },
            _optionsMonitor, _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        var links = context.Response.Headers.Link.ToArray();
        Assert.Contains(links,
            l => l != null && l.Contains("style.css") && l.Contains("preload"));
        Assert.Contains(links,
            l => l != null && l.Contains("hero.jpg") && l.Contains("preload"));
    }

    [Theory]
    [InlineData(null, 0, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.MustRevalidate, 0, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.Public, 0, true)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.SMaxagePresent, 60, true)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.SMaxagePresent, 0, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.SMaxagePresent
                | CacheControlCcFlags.SMaxageRepeated, 60, false)]
    public async Task MissPath_CookieRequest_GetsCachedEarlyHintsOnlyFromAPageMarkedShared(
        int? pageOriginCcFlags, int pageOriginSMaxage, bool expectHints)
    {
        var hintsData = Encoding.UTF8.GetBytes("/css/style.css");
        var cache = new EarlyHintsPageSpeedCache(
            hintsData, pageOriginCcFlags is int f ? (ushort)f : null,
            (uint)pageOriginSMaxage);

        var middleware = new PageSpeedMiddleware(
            ctx =>
            {
                ctx.Response.StatusCode = 200;
                ctx.Response.ContentType = "text/html";
                var body = Encoding.UTF8.GetBytes("<html>fresh</html>");
                return ctx.Response.Body.WriteAsync(body, 0, body.Length);
            },
            _optionsMonitor, _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.Cookie = "session=abc";
        context.Response.Body = new MemoryStream();

        await middleware.InvokeAsync(context);

        var links = context.Response.Headers.Link.ToArray();
        Assert.Equal(expectHints,
            links.Any(l => l != null && l.Contains("style.css")));
    }

    [Theory]
    [InlineData("http", "example.com", "/page", "", "/page", "http")]
    [InlineData("https", "example.com", "/page", "?v=1", "/page?v=1", "https")]
    [InlineData("https", "example.com:8080", "/app/page", "", "/app/page", "https")]
    public async Task CacheUrl_IsPathAndQueryOnly_NoSchemeOrHost(
        string scheme, string host, string path, string query,
        string expectedUrl, string expectedScheme)
    {
        // Arrange: use a URL-capturing cache to verify the URL format.
        var cache = new UrlCapturingCache();
        var middleware = new PageSpeedMiddleware(
            ctx =>
            {
                ctx.Response.StatusCode = 200;
                ctx.Response.ContentType = "text/html";
                var body = Encoding.UTF8.GetBytes("<html>ok</html>");
                return ctx.Response.Body.WriteAsync(body, 0, body.Length);
            },
            _optionsMonitor, _htmlProcessor, cache, _notifier, _logger);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Scheme = scheme;
        context.Request.Host = new HostString(host);
        context.Request.Path = path;
        context.Request.QueryString = new QueryString(
            string.IsNullOrEmpty(query) ? "" : query);
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        // Act
        await middleware.InvokeAsync(context);

        // Assert: URL passed to cache must be path+query only,
        // matching nginx module format (no scheme://, no hostname).
        Assert.Equal(expectedUrl, cache.CapturedUrl);
        Assert.DoesNotContain("://", cache.CapturedUrl ?? "");
        // Assert: scheme is passed through correctly.
        Assert.Equal(expectedScheme, cache.CapturedScheme);
    }

    /// <summary>
    /// Fake IHttpWebSocketFeature that reports IsWebSocketRequest = true.
    /// </summary>
    private sealed class FakeWebSocketFeature : IHttpWebSocketFeature
    {
        public bool IsWebSocketRequest => true;

        public Task<System.Net.WebSockets.WebSocket> AcceptAsync(
            WebSocketAcceptContext context) =>
            throw new NotImplementedException();
    }

    /// <summary>
    /// Testable IReadResult that uses managed memory (no native library).
    /// </summary>
    private sealed class FakeReadResult : IReadResult
    {
        public ReadOnlyMemory<byte> ContentMemory { get; init; }
        public uint Mask { get; init; }
        public PageSpeedContentType ContentType { get; init; }
        public string? OriginContentType { get; init; }
        // Entries these tests describe were written with their response
        // headers checked (nothing to carry) from an anonymous request, as
        // the middleware writes them.
        private readonly byte _flags;
        public byte Flags
        {
            get => (byte)(_flags | CacheFlags.HeadersClassified | CacheFlags.AnonymousRecorded);
            init => _flags = value;
        }
        public ushort OriginCcFlags { get; init; }
        public uint OriginSMaxage { get; init; }

        // Mirrors ReadResult.NeedsRevalidation: derive from Flags so
        // tests can set either the init-prop or the Flags bit.
        private readonly bool _needsRevalidationOverride;
        public bool NeedsRevalidation
        {
            get => _needsRevalidationOverride
                || (Flags & CacheFlags.NeedsRevalidation) != 0;
            init => _needsRevalidationOverride = value;
        }

        // Mirrors ReadResult.RevalidationRequired: for a private cache
        // (ASP.NET in-process), proxy-revalidate and s-maxage are ignored.
        public bool RevalidationRequired =>
            (OriginCcFlags & (CacheControlCcFlags.NoCache | CacheControlCcFlags.MustRevalidate)) != 0;

        // Fakes have no native mmap borrow, so there is never a
        // lease to renew — mirrors the real impl's RAM-hit/disabled path.
        public bool RenewLease() => false;

        public uint OriginContentLength { get; init; }

        // Mirrors ReadResult.IsWorkerProcessed: the kFlagWorkerProcessed bit
        // (0x02). Tests can set the init-prop directly or via the Flags bit.
        private const byte WorkerProcessedBit = 0x02;
        private readonly bool _isWorkerProcessedOverride;
        public bool IsWorkerProcessed
        {
            get => _isWorkerProcessedOverride
                || (Flags & WorkerProcessedBit) != 0;
            init => _isWorkerProcessedOverride = value;
        }

        public uint CacheInsertedAt { get; init; }
        public void Dispose() { }
    }

    /// <summary>
    /// Fake IPageSpeedCache that always returns null (miss).
    /// </summary>
    private sealed class NullPageSpeedCache : IPageSpeedCache
    {
        WeAmp.PageSpeed.Native.SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException(
                "Test fake has no native handle");

        public IReadResult? ReadBest(
            string url, string hostname, string scheme, uint mask) => null;

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled) => null;

        public int ReadSharedConfigAgentEntitled() => 0;

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId) => null;

        public IReadResult? ReadEarlyHints(
            string url, string hostname, string scheme) => null;

        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId,
            ulong contentLength) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheStats GetStats() =>
            new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) { }

        public void Dispose() { }
    }

    /// <summary>
    /// Fake IPageSpeedCache that returns a configurable IReadResult on ReadBest.
    /// </summary>
    private sealed class HitPageSpeedCache : IPageSpeedCache
    {
        private readonly IReadResult _result;

        public HitPageSpeedCache(IReadResult result) => _result = result;

        // The worker's published agent optimization flag.
        public int AgentEntitled { get; init; }

        WeAmp.PageSpeed.Native.SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException(
                "Test fake has no native handle");

        public IReadResult? ReadBest(
            string url, string hostname, string scheme, uint mask) => _result;

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled) => null;

        public int ReadSharedConfigAgentEntitled() => AgentEntitled;

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId) => null;

        public IReadResult? ReadEarlyHints(
            string url, string hostname, string scheme) => null;

        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId,
            ulong contentLength) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheStats GetStats() =>
            new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) { }

        public void Dispose() { }
    }

    /// <summary>
    /// Fake IPageSpeedCache that returns null for ReadBest (miss) but
    /// returns early hints from ReadEarlyHints.
    /// </summary>
    private sealed class EarlyHintsPageSpeedCache : IPageSpeedCache
    {
        private readonly byte[] _hints;
        private readonly ushort? _pageOriginCcFlags;
        private readonly uint _pageOriginSMaxage;

        // pageOriginCcFlags / pageOriginSMaxage: the recorded page's origin
        // Cache-Control state, or null for no recorded page.
        public EarlyHintsPageSpeedCache(
            byte[] hints, ushort? pageOriginCcFlags = null, uint pageOriginSMaxage = 0)
        {
            _hints = hints;
            _pageOriginCcFlags = pageOriginCcFlags;
            _pageOriginSMaxage = pageOriginSMaxage;
        }

        WeAmp.PageSpeed.Native.SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException(
                "Test fake has no native handle");

        public IReadResult? ReadBest(
            string url, string hostname, string scheme, uint mask) => null;

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled) => null;

        public int ReadSharedConfigAgentEntitled() => 0;

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId) =>
            _pageOriginCcFlags is ushort flags && alternateId == 0x08
                ? new FakeReadResult
                {
                    ContentType = PageSpeedContentType.Html,
                    OriginCcFlags = flags,
                    OriginSMaxage = _pageOriginSMaxage,
                }
                : null;

        public IReadResult? ReadEarlyHints(
            string url, string hostname, string scheme) =>
            new FakeReadResult
            {
                ContentMemory = _hints,
                ContentType = PageSpeedContentType.Html,
            };

        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId,
            ulong contentLength) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheStats GetStats() =>
            new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) { }

        public void Dispose() { }
    }

    /// <summary>
    /// Fake IPageSpeedCache that captures the URL passed to ReadBest.
    /// </summary>
    private sealed class UrlCapturingCache : IPageSpeedCache
    {
        public string? CapturedUrl { get; private set; }
        public string? CapturedHostname { get; private set; }
        public string? CapturedScheme { get; private set; }

        WeAmp.PageSpeed.Native.SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException(
                "Test fake has no native handle");

        public IReadResult? ReadBest(
            string url, string hostname, string scheme, uint mask)
        {
            CapturedUrl = url;
            CapturedHostname = hostname;
            CapturedScheme = scheme;
            return null;
        }

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled) => null;

        public int ReadSharedConfigAgentEntitled() => 0;

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId) => null;

        public IReadResult? ReadEarlyHints(
            string url, string hostname, string scheme) => null;

        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId,
            ulong contentLength) =>
            throw new NotSupportedException(
                "Test fake does not support writes");

        public CacheStats GetStats() =>
            new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) { }

        public void Dispose() { }
    }
}

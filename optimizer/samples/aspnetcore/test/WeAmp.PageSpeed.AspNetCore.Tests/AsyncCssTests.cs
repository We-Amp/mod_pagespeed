// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NSubstitute;
using WeAmp.PageSpeed.Native;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// CSP-safe async-CSS parity for the in-process .NET middleware: the same
/// native transform + the same loader the nginx front-end uses. Keeps .NET a
/// first-class citizen alongside nginx.
/// </summary>
public class AsyncCssTests
{
    // The loader getters surfaced over the C ABI must match the reserved path
    // and be CSP-safe (external script only). Both front-ends serve THIS JS.
    [Fact]
    public void NativeGetters_ReturnLoaderPathAndCspSafeJs()
    {
        // Content-addressed path: assert SHAPE, not a fixed literal (a loader
        // change changes the embedded hash, hence the path). The reserved prefix
        // and .js suffix are the stable contract the nginx `/pagespeed_static/`
        // location and the immutable/1yr cache policy depend on.
        var path = NativePageSpeed.AsyncCssLoaderPath();
        Assert.False(string.IsNullOrEmpty(path));
        Assert.StartsWith("/pagespeed_static/async_css.", path);
        Assert.EndsWith(".js", path);

        var js = NativePageSpeed.AsyncCssLoaderJs();
        Assert.False(string.IsNullOrEmpty(js));
        Assert.Contains("data-pagespeed-async", js);
        Assert.DoesNotContain("onload=", js);  // external script, not inline
    }

    // The NativeHtmlConfig struct must stay layout-matched with the C ABI
    // ps_html_config_t, with enable_async_css appended at offset 120 and
    // scheme at offset 128 (size 136).
    // HtmlConfigInit is a real P/Invoke that writes the native defaults THROUGH
    // the managed layout, so this exercises the actual struct marshaling: a
    // wrong size/offset would corrupt StructSize or the field value.
    [Fact]
    public void NativeHtmlConfig_LayoutMatchesAbi_AndAsyncCssDefaultsOff()
    {
        Assert.Equal(136, Marshal.SizeOf<NativeHtmlConfig>());
        Assert.Equal(120, (int)Marshal.OffsetOf<NativeHtmlConfig>(
            nameof(NativeHtmlConfig.EnableAsyncCss)));
        Assert.Equal(128, (int)Marshal.OffsetOf<NativeHtmlConfig>(
            nameof(NativeHtmlConfig.Scheme)));

        var config = default(NativeHtmlConfig);
        NativePageSpeed.HtmlConfigInit(ref config);
        // The legacy initializer stamps the 128-byte size and leaves Scheme
        // untouched; HtmlProcessor claims the full 136 itself.
        Assert.Equal((nuint)128, config.StructSize);
        Assert.Equal(IntPtr.Zero, config.Scheme);
        Assert.Equal(1, config.EnableCriticalCss);     // known default (sanity)
        Assert.Equal(0, config.EnableAsyncCss);        // opt-in, off by default

        config.EnableAsyncCss = 1;
        Assert.Equal(1, config.EnableAsyncCss);
    }

    // The middleware serves the loader at EXACTLY the native getter's path (the
    // content-addressed single source of truth — this is also the drift guard:
    // the middleware caches that path at startup, so serving it here proves the
    // managed and native paths agree), with an immutable cache, and the bytes
    // are IDENTICAL to the native constant (nginx and .NET serve the same loader).
    [Fact]
    public async Task Middleware_ServesAsyncCssLoaderByteIdentical()
    {
        var middleware = CreateMiddleware(_ => Task.CompletedTask);
        var ctx = new DefaultHttpContext();
        ctx.Request.Method = HttpMethods.Get;
        // Derive the path from the native getter, never hardcode the hash.
        ctx.Request.Path = NativePageSpeed.AsyncCssLoaderPath();
        var body = new MemoryStream();
        ctx.Response.Body = body;

        await middleware.InvokeAsync(ctx);

        Assert.Equal(StatusCodes.Status200OK, ctx.Response.StatusCode);
        Assert.Equal("text/javascript; charset=utf-8", ctx.Response.ContentType);
        // Content-addressed path => immutable + 1-year max-age (lockstep with
        // the nginx module). A loader change yields a new path, so a stale
        // year-long cache is impossible.
        var cc = ctx.Response.Headers.CacheControl.ToString();
        Assert.Contains("max-age=31536000", cc);
        Assert.Contains("immutable", cc);
        // Provenance marker the deploy smoke asserts (parity with nginx).
        Assert.Equal("async-css-loader",
            ctx.Response.Headers["X-PageSpeed"].ToString());
        var served = Encoding.UTF8.GetString(body.ToArray());
        Assert.Equal(NativePageSpeed.AsyncCssLoaderJs(), served);
    }

    [Fact]
    public async Task Middleware_LoaderPath_DoesNotCallNext()
    {
        var nextCalled = false;
        var middleware = CreateMiddleware(_ =>
        {
            nextCalled = true;
            return Task.CompletedTask;
        });
        var ctx = new DefaultHttpContext();
        ctx.Request.Method = HttpMethods.Get;
        ctx.Request.Path = NativePageSpeed.AsyncCssLoaderPath();
        ctx.Response.Body = new MemoryStream();

        await middleware.InvokeAsync(ctx);

        Assert.False(nextCalled);
    }

    private static PageSpeedMiddleware CreateMiddleware(RequestDelegate next)
    {
        var options = new PageSpeedOptions { Enabled = true };
        var monitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        monitor.CurrentValue.Returns(options);

        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        var notifier = new WorkerNotificationService(
            notifierOptions,
            new WeAmp.PageSpeed.AspNetCore.Internal.InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());

        // The loader path short-circuits at the top of InvokeAsync before any
        // cache access, so the cache is never dereferenced here. (IPageSpeedCache
        // exposes an internal Handle that NSubstitute cannot proxy.)
        return new PageSpeedMiddleware(
            next, monitor, Substitute.For<IHtmlProcessor>(),
            cache: null!, notifier,
            Substitute.For<ILogger<PageSpeedMiddleware>>());
    }
}

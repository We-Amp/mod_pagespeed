// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Collections.Concurrent;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// A request that carries a cookie shares the cache with other visitors only
/// when the origin opted in: a page is recorded for it, and served to it from
/// the cache, only when the origin marked the page <c>public</c> or gave it a
/// valid <c>s-maxage</c>; a stylesheet, script or image is recorded for it
/// only with the origin's own freshness lifetime.
/// Runs the standalone middleware (no worker) with the real native cache.
/// Requires the native library: skipped when it is absent locally, failed
/// when it is absent on CI or with PAGESPEED_E2E_REQUIRE_NATIVE=1.
/// </summary>
public class CookieRequestRecordTests : IDisposable
{
    // The page links a stylesheet the cache already holds, so the middleware
    // optimizes it (an unchanged page is passed through, never stored).
    private const string PageHtml =
        "<!doctype html><html><head>" +
        "<link rel=\"stylesheet\" href=\"/open.css\">" +
        "</head><body><header>Top</header><main><p>Hello</p></main>" +
        "</body></html>";

    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_cookie_{Guid.NewGuid():N}");

    private readonly ConcurrentDictionary<string, int> _served = new();

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
    }

    // A missing native library fails the test: the theories are skipped up
    // front when it is absent locally (NativeLibTheory), and required on CI.
    private async Task<WebApplication> StartAsync(Action<WebApplication> map)
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.WebHost.UseTestServer();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddPageSpeed(opts =>
        {
            opts.Cache.VolumePath = Path.Combine(_dir, "volume.dat");
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = false;  // standalone: no worker
        });
        var app = builder.Build();

        app.UsePageSpeed();
        app.MapGet("/open.css", (HttpContext ctx) =>
        {
            ctx.Response.Headers.CacheControl = "public, max-age=3600";
            return Results.Text("header{background:#0a0a0a}", "text/css");
        });
        map(app);

        await app.StartAsync();
        // An anonymous visit records the page's stylesheet.
        await SendAsync(app, "/open.css");
        return app;
    }

    private void Route(WebApplication app, string path, string contentType,
        string? cacheControl, byte[] body, string? expires = null)
    {
        app.MapGet(path, (HttpContext ctx) =>
        {
            _served.AddOrUpdate(path, 1, (_, n) => n + 1);
            if (cacheControl != null) ctx.Response.Headers.CacheControl = cacheControl;
            if (expires != null) ctx.Response.Headers.Expires = expires;
            return Results.Bytes(body, contentType);
        });
    }

    private void Page(WebApplication app, string path, string? cacheControl,
        string contentType = "text/html") =>
        Route(app, path, contentType, cacheControl,
            System.Text.Encoding.UTF8.GetBytes(PageHtml));

    private int Served(string path) => _served.TryGetValue(path, out var n) ? n : 0;

    // One request, with the given raw request headers (an empty value is
    // sent as an empty header). Returns the X-PageSpeed value.
    private static async Task<string> SendAsync(WebApplication app, string path,
        params (string Name, string Value)[] headers)
    {
        var ctx = await app.GetTestServer().SendAsync(c =>
        {
            c.Request.Method = "GET";
            c.Request.Scheme = "https";
            c.Request.Host = new HostString("localhost");
            c.Request.Path = path;
            c.Request.Headers.Accept = "text/html";
            foreach (var (name, value) in headers) c.Request.Headers[name] = value;
        });
        Assert.Equal(StatusCodes.Status200OK, ctx.Response.StatusCode);
        // The body ends when the pipeline has returned, after the
        // middleware's cache write.
        await new StreamReader(ctx.Response.Body).ReadToEndAsync();
        return ctx.Response.Headers["X-PageSpeed"].ToString();
    }

    private static readonly (string, string) SessionCookie = ("Cookie", "session=abc");

    // ── Pages fetched for a cookie-bearing request ──────────────────

    [NativeLibTheory]
    [InlineData(null)]
    [InlineData("max-age=60")]
    [InlineData("max-age=60, must-revalidate")]
    [InlineData("s-maxage=0")]
    [InlineData("s-maxage=abc")]
    [InlineData("s-maxage=-5")]
    [InlineData("s-maxage=60, s-maxage=120")]
    [InlineData("max-age=60, s-maxage=60, S-MAXAGE=60")]
    public async Task PageForCookieRequest_WithoutPublicOrValidSMaxage_IsNotShared(
        string? cacheControl)
    {
        var app = await StartAsync(a => Page(a, "/p", cacheControl));
        try
        {
            Assert.NotEqual("HIT", await SendAsync(app, "/p", SessionCookie));
            Assert.NotEqual("HIT", await SendAsync(app, "/p"));
            Assert.Equal(2, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("Cookie", "")]
    [InlineData("Cookie2", "$Version=1")]
    [InlineData("cookie", "a=b")]
    public async Task PageForRequestWithAnyCookieHeader_IsNotShared(string name, string value)
    {
        var app = await StartAsync(a => Page(a, "/p", "max-age=60"));
        try
        {
            Assert.NotEqual("HIT", await SendAsync(app, "/p", (name, value)));
            Assert.NotEqual("HIT", await SendAsync(app, "/p"));
            Assert.Equal(2, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("TEXT/HTML; charset=utf-8")]
    [InlineData("Text/Html")]
    public async Task PageForCookieRequest_ContentTypeMatchIsCaseInsensitive(string contentType)
    {
        var app = await StartAsync(a => Page(a, "/p", "max-age=60", contentType));
        try
        {
            await SendAsync(app, "/p", SessionCookie);
            Assert.NotEqual("HIT", await SendAsync(app, "/p"));
            Assert.Equal(2, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("public, max-age=60")]
    [InlineData("s-maxage=60")]
    [InlineData("max-age=60, s-maxage=120")]
    [InlineData("s-maxage=\"60\"")]
    [InlineData("s-maxage=60, s-maxage=120, public")]
    public async Task PageForCookieRequest_MarkedPublicOrSMaxage_IsShared(string cacheControl)
    {
        var app = await StartAsync(a => Page(a, "/p", cacheControl));
        try
        {
            Assert.NotEqual("HIT", await SendAsync(app, "/p", SessionCookie));
            Assert.Equal("HIT", await SendAsync(app, "/p"));
            Assert.Equal("HIT", await SendAsync(app, "/p", ("Cookie", "session=other")));
            Assert.Equal(1, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Stored pages and cookie-bearing requests ────────────────────

    [NativeLibTheory]
    [InlineData("Cookie", "session=abc")]
    [InlineData("Cookie", "")]
    [InlineData("Cookie2", "$Version=1")]
    public async Task StoredPageWithoutMark_IsNotServedToCookieRequest(string name, string value)
    {
        var app = await StartAsync(a => Page(a, "/p", "max-age=60, must-revalidate"));
        try
        {
            await SendAsync(app, "/p");
            Assert.Equal("HIT", await SendAsync(app, "/p"));  // control
            Assert.NotEqual("HIT", await SendAsync(app, "/p", (name, value)));
            Assert.Equal(2, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("s-maxage=0")]
    [InlineData("s-maxage=abc")]
    [InlineData("s-maxage=60, s-maxage=120")]
    public async Task StoredPageWithInvalidSMaxage_IsNotServedToCookieRequest(string cacheControl)
    {
        var app = await StartAsync(a => Page(a, "/p", cacheControl));
        try
        {
            await SendAsync(app, "/p");
            Assert.Equal("HIT", await SendAsync(app, "/p"));  // control
            Assert.NotEqual("HIT", await SendAsync(app, "/p", SessionCookie));
            Assert.Equal(2, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("public, max-age=60")]
    [InlineData("s-maxage=60")]
    public async Task StoredPageMarkedPublicOrSMaxage_IsServedToCookieRequest(string cacheControl)
    {
        var app = await StartAsync(a => Page(a, "/p", cacheControl));
        try
        {
            await SendAsync(app, "/p");
            Assert.Equal("HIT", await SendAsync(app, "/p", SessionCookie));
            Assert.Equal(1, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Stylesheets, scripts and images ─────────────────────────────

    public static IEnumerable<object[]> Assets()
    {
        yield return new object[] { "/a.css", "text/css" };
        yield return new object[] { "/a.js", "application/javascript" };
        yield return new object[] { "/a.gif", "image/gif" };
    }

    private static readonly byte[] AssetBody =
        System.Text.Encoding.UTF8.GetBytes("header{background:#0a0a0a}");

    [NativeLibTheory]
    [MemberData(nameof(Assets))]
    public async Task AssetForCookieRequest_WithOriginLifetime_IsShared(string path, string type)
    {
        var app = await StartAsync(a => Route(a, path, type, "max-age=3600", AssetBody));
        try
        {
            await SendAsync(app, path, SessionCookie);
            Assert.Equal("HIT", await SendAsync(app, path));
            Assert.Equal(1, Served(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [MemberData(nameof(Assets))]
    public async Task AssetForCookieRequest_WithOriginExpires_IsShared(string path, string type)
    {
        var expires = DateTimeOffset.UtcNow.AddHours(1).ToString("r");
        var app = await StartAsync(a => Route(a, path, type, null, AssetBody, expires));
        try
        {
            await SendAsync(app, path, SessionCookie);
            Assert.Equal("HIT", await SendAsync(app, path));
            Assert.Equal(1, Served(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    public static IEnumerable<object[]> AssetsWithoutLifetime()
    {
        foreach (var asset in Assets())
        {
            foreach (var cc in new string?[] { null, "public", "max-age=0", "s-maxage=0", "no-cache" })
                yield return new object[] { asset[0], asset[1], cc! };
        }
    }

    [NativeLibTheory]
    [MemberData(nameof(AssetsWithoutLifetime))]
    public async Task AssetForCookieRequest_WithoutOriginLifetime_IsNotShared(
        string path, string type, string? cacheControl)
    {
        var app = await StartAsync(a => Route(a, path, type, cacheControl, AssetBody));
        try
        {
            await SendAsync(app, path, SessionCookie);
            Assert.NotEqual("HIT", await SendAsync(app, path));
            Assert.Equal(2, Served(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [MemberData(nameof(Assets))]
    public async Task AssetForCookieRequest_ExpiresAlongsideCacheControl_IsNoLifetime(
        string path, string type)
    {
        var expires = DateTimeOffset.UtcNow.AddHours(1).ToString("r");
        var app = await StartAsync(a => Route(a, path, type, "public", AssetBody, expires));
        try
        {
            await SendAsync(app, path, SessionCookie);
            Assert.NotEqual("HIT", await SendAsync(app, path));
            Assert.Equal(2, Served(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [MemberData(nameof(Assets))]
    public async Task StoredAsset_IsServedToCookieRequest(string path, string type)
    {
        var app = await StartAsync(a => Route(a, path, type, "public", AssetBody));
        try
        {
            await SendAsync(app, path);
            Assert.Equal("HIT", await SendAsync(app, path, SessionCookie));
            Assert.Equal(1, Served(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Authorization handling is unchanged ─────────────────────────

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PageForAuthorizationRequest_IsNotSharedEvenWhenMarkedPublic(bool withCookie)
    {
        var app = await StartAsync(a => Page(a, "/p", "public, max-age=60"));
        try
        {
            var headers = withCookie
                ? new[] { ("Authorization", "Basic dXNlcjpwYXNz"), SessionCookie }
                : new[] { ("Authorization", "Basic dXNlcjpwYXNz") };
            await SendAsync(app, "/p", headers);
            Assert.NotEqual("HIT", await SendAsync(app, "/p"));
            Assert.Equal(2, Served("/p"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }
}

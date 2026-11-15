// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NSubstitute;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The headers an application sets are kept with the cache entry and sent
/// again on every response served from the cache: 200 hits, 304s, hits that
/// trigger revalidation, and hits that also carry early-hint Link headers.
/// Headers that depend on the request or the connection are never stored or
/// replayed.
/// </summary>
public class ResponseHeaderReplayTests
{
    private static readonly byte[] Body = Encoding.UTF8.GetBytes("body { color: red }");

    private static byte[] EncodeBlock(params (string Name, string Value)[] fields)
    {
        var ms = new MemoryStream();
        ms.WriteByte(1);
        ms.WriteByte((byte)(fields.Length >> 8));
        ms.WriteByte((byte)fields.Length);
        foreach (var (n, v) in fields)
        {
            var nb = Encoding.Latin1.GetBytes(n);
            var vb = Encoding.Latin1.GetBytes(v);
            ms.WriteByte((byte)nb.Length);
            ms.WriteByte((byte)(vb.Length >> 8));
            ms.WriteByte((byte)vb.Length);
            ms.Write(nb);
            ms.Write(vb);
        }
        return ms.ToArray();
    }

    private sealed class Result : IReadResult
    {
        public ReadOnlyMemory<byte> ContentMemory { get; init; }
        public uint Mask { get; init; }
        public PageSpeedContentType ContentType { get; init; } = PageSpeedContentType.Css;
        public string? OriginContentType { get; init; } = "text/css";
        public byte Flags { get; init; }
        public bool NeedsRevalidation => (Flags & CacheFlags.NeedsRevalidation) != 0;
        public ushort OriginCcFlags => 0;
        public bool RevalidationRequired => false;
        public uint CacheInsertedAt { get; init; } = 1_700_000_000;
        public uint OriginContentLength => 0;
        public bool IsWorkerProcessed => false;
        public bool RenewLease() => false;
        public void Dispose() { }
    }

    private sealed class FakeCache : IPageSpeedCache
    {
        public IReadResult? Hit { get; set; }
        public byte[]? Block { get; set; }
        public byte[]? EarlyHints { get; set; }
        public HeadersSidecarVerdict Verdict { get; set; } = HeadersSidecarVerdict.SwapEligible;
        public bool Stored { get; set; } = true;
        public List<IReadOnlyList<KeyValuePair<string, string>>> Offered { get; } = new();
        public int BeginWriteCalls { get; private set; }

        WeAmp.PageSpeed.Native.SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException();

        public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask) => Hit;
        public IReadResult? ReadBestAgent(string url, string hostname, string scheme, uint mask, int e) => null;
        public int ReadSharedConfigAgentEntitled() => 0;

        public IReadResult? ReadAlternate(string url, string hostname, string scheme, byte id) =>
            id == SentinelId.HeadersSidecar && Block != null
                ? new Result { ContentMemory = Block }
                : null;

        public IReadResult? ReadEarlyHints(string url, string hostname, string scheme) =>
            EarlyHints == null ? null : new Result { ContentMemory = EarlyHints };

        public CacheWriter BeginWrite(string url, string hostname, string scheme, CacheWriteParams p)
        {
            BeginWriteCalls++;
            throw new NotSupportedException();
        }

        public CacheWriter BeginWriteSentinel(string url, string hostname, string scheme, byte id, ulong len) =>
            throw new NotSupportedException();

        public HeadersSidecarWriteResult WriteHeadersSidecar(
            string url, string hostname, string scheme,
            IReadOnlyList<KeyValuePair<string, string>> headers)
        {
            Offered.Add(headers);
            return new(Verdict, Stored);
        }

        public CacheStats GetStats() => new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        public void RecordServeHit(PageSpeedContentType t, uint o, ulong b, uint m) { }
        public void Dispose() { }
    }

    private static PageSpeedMiddleware Create(IPageSpeedCache cache, RequestDelegate next)
    {
        var options = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        options.CurrentValue.Returns(new PageSpeedOptions { Enabled = true });
        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        var notifier = new WorkerNotificationService(
            notifierOptions,
            new WeAmp.PageSpeed.AspNetCore.Internal.InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());
        return new PageSpeedMiddleware(
            next, options, Substitute.For<IHtmlProcessor>(), cache, notifier,
            Substitute.For<ILogger<PageSpeedMiddleware>>());
    }

    private static DefaultHttpContext NewContext(string path = "/site.css")
    {
        var ctx = new DefaultHttpContext();
        ctx.Request.Method = "GET";
        ctx.Request.Path = path;
        ctx.Request.Scheme = "https";
        ctx.Request.Host = new HostString("example.test");
        ctx.Response.Body = new MemoryStream();
        return ctx;
    }

    private static readonly (string, string)[] AppHeaders =
    {
        ("Content-Security-Policy", "default-src 'self'"),
        ("Access-Control-Allow-Origin", "https://app.example"),
        ("X-Content-Type-Options", "nosniff"),
        ("Referrer-Policy", "no-referrer"),
        ("Cross-Origin-Opener-Policy", "same-origin"),
        ("Cross-Origin-Embedder-Policy", "require-corp"),
        ("Cross-Origin-Resource-Policy", "same-site"),
        ("Permissions-Policy", "camera=()"),
        ("Vary", "User-Agent"),
    };

    // How the middleware records an entry: headers checked, from an
    // anonymous request.
    private const byte Recorded = CacheFlags.HeadersClassified | CacheFlags.AnonymousRecorded;

    private static FakeCache HitCache(byte flags = Recorded | CacheFlags.HeadersSidecarRequired,
        (string, string)[]? block = null) =>
        new()
        {
            Hit = new Result { ContentMemory = Body, Flags = flags },
            Block = EncodeBlock(block ?? AppHeaders),
        };

    private static Task<DefaultHttpContext> Run(FakeCache cache, DefaultHttpContext? ctx = null,
        RequestDelegate? next = null) => RunAsync(cache, ctx ?? NewContext(), next);

    private static async Task<DefaultHttpContext> RunAsync(FakeCache cache, DefaultHttpContext ctx, RequestDelegate? next)
    {
        await Create(cache, next ?? (_ => Task.CompletedTask)).InvokeAsync(ctx);
        return ctx;
    }

    private static void AssertAppHeaders(IHeaderDictionary h)
    {
        foreach (var (name, value) in AppHeaders.Where(x => x.Item1 != "Vary"))
            Assert.Equal(value, h[name].ToString());
        var vary = h.Vary.ToString();
        Assert.Contains("User-Agent", vary);
        Assert.Contains("Accept-Encoding", vary);
    }

    [Fact]
    public async Task CacheHit_CarriesApplicationHeaders_AndCorrectLength()
    {
        var ctx = await Run(HitCache());
        Assert.Equal(200, ctx.Response.StatusCode);
        Assert.Equal("HIT", ctx.Response.Headers["X-PageSpeed"].ToString());
        AssertAppHeaders(ctx.Response.Headers);
        Assert.Equal(Body.Length, ctx.Response.ContentLength);
        Assert.Equal(Body, ((MemoryStream)ctx.Response.Body).ToArray());
    }

    [Fact]
    public async Task CacheHit_StoredDateIsNeverReplayed_EntryIsRefetched()
    {
        var cache = HitCache(block: new[] { ("Date", "Mon, 01 Jan 2001 00:00:00 GMT") });
        var reached = false;
        var ctx = await Run(cache, NewContext(), c => { reached = true; return Task.CompletedTask; });
        Assert.True(reached);
        Assert.False(ctx.Response.Headers.ContainsKey("Date"));
    }

    [Fact]
    public async Task CacheHit_IfNoneMatch_304CarriesApplicationHeaders()
    {
        var ctx = NewContext();
        ctx.Request.Headers.IfNoneMatch = PageSpeedMiddleware.ComputeContentETag(Body);
        await Run(HitCache(), ctx);
        Assert.Equal(304, ctx.Response.StatusCode);
        AssertAppHeaders(ctx.Response.Headers);
        Assert.Empty(((MemoryStream)ctx.Response.Body).ToArray());
    }

    [Fact]
    public async Task CacheHit_IfModifiedSince_304CarriesApplicationHeaders()
    {
        var ctx = NewContext();
        ctx.Request.Headers.IfModifiedSince =
            DateTimeOffset.FromUnixTimeSeconds(1_700_000_100).ToString("R");
        await Run(HitCache(), ctx);
        Assert.Equal(304, ctx.Response.StatusCode);
        AssertAppHeaders(ctx.Response.Headers);
    }

    [Fact]
    public async Task CacheHit_NeedingRevalidation_CarriesApplicationHeaders()
    {
        var cache = HitCache(Recorded | CacheFlags.HeadersSidecarRequired
            | CacheFlags.NeedsRevalidation);
        var ctx = await Run(cache);
        Assert.Equal(200, ctx.Response.StatusCode);
        AssertAppHeaders(ctx.Response.Headers);
    }

    [Fact]
    public async Task CacheHit_WithEarlyHints_KeepsApplicationLinkAndHintLinks()
    {
        var cache = new FakeCache
        {
            Hit = new Result
            {
                ContentMemory = Body,
                ContentType = PageSpeedContentType.Html,
                OriginContentType = "text/html",
                Flags = Recorded | CacheFlags.HeadersSidecarRequired,
            },
            Block = EncodeBlock(("Link", "</app.css>; rel=preload; as=style"), ("Vary", "Save-Data")),
            EarlyHints = Encoding.UTF8.GetBytes("/hint.css\n"),
        };
        var ctx = await Run(cache, NewContext("/page"));
        var links = ctx.Response.Headers.Link.ToArray();
        Assert.Contains("</app.css>; rel=preload; as=style", links);
        Assert.Contains(links, l => l!.Contains("/hint.css"));
        Assert.Contains("Save-Data", ctx.Response.Headers.Vary.ToString());
        Assert.Contains("User-Agent", ctx.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task CacheHit_DoesNotRepeatAHeaderAnEarlierMiddlewareSet()
    {
        var ctx = NewContext();
        ctx.Response.Headers["X-Content-Type-Options"] = "nosniff";
        await Run(HitCache(), ctx);
        Assert.Equal("nosniff", ctx.Response.Headers["X-Content-Type-Options"].ToString());
        Assert.Single(ctx.Response.Headers["X-Content-Type-Options"]);
    }

    [Fact]
    public async Task CacheHit_StoredCookieIsNeverReplayed_ForAnotherUser()
    {
        // Entry whose block was damaged or foreign: it holds user A's cookie.
        var cache = HitCache(block: new[]
        {
            ("Set-Cookie", "session=user-a"),
            ("X-Content-Type-Options", "nosniff"),
        });
        var ctx = await Run(cache, NewContext(), c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Append("Set-Cookie", "session=user-b");
            return c.Response.WriteAsync("user b");
        });
        var cookies = ctx.Response.Headers.SetCookie.ToArray();
        Assert.Equal(new[] { "session=user-b" }, cookies);
        Assert.Equal(0, cache.BeginWriteCalls);
    }

    [Fact]
    public async Task Miss_ResponseWithSetCookie_IsNeverOfferedForStorage()
    {
        var cache = new FakeCache();
        var ctx = await Run(cache, NewContext(), c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Append("Set-Cookie", "session=user-a");
            return c.Response.WriteAsync("user a");
        });
        Assert.Empty(cache.Offered);
        Assert.Equal(0, cache.BeginWriteCalls);
        Assert.Equal(new[] { "session=user-a" }, ctx.Response.Headers.SetCookie.ToArray());
    }

    [Fact]
    public async Task CacheHit_EntryWithoutStoredHeadersFlag_IsRefetched()
    {
        var cache = HitCache(flags: 0);
        var reached = false;
        await Run(cache, NewContext(), c => { reached = true; return Task.CompletedTask; });
        Assert.True(reached);
    }

    [Theory]
    [InlineData(CacheFlags.HeadersClassified)]
    [InlineData(CacheFlags.HeadersClassified | CacheFlags.HeadersSidecarRequired)]
    public async Task CacheHit_EntryWithoutAnonymousRecordStamp_IsRefetched(int flags)
    {
        var cache = HitCache(flags: (byte)flags);
        var reached = false;
        var ctx = await Run(cache, NewContext(), c => { reached = true; return Task.CompletedTask; });
        Assert.True(reached);
        Assert.NotEqual("HIT", ctx.Response.Headers["X-PageSpeed"].ToString());
    }

    [Fact]
    public async Task CacheHit_RequiredBlockMissing_IsRefetched()
    {
        var cache = HitCache();
        cache.Block = null;
        var reached = false;
        await Run(cache, NewContext(), c => { reached = true; return Task.CompletedTask; });
        Assert.True(reached);
    }

    [Fact]
    public async Task CacheHit_ClassifiedWithNothingToCarry_IsServed()
    {
        var cache = HitCache(flags: Recorded);
        var ctx = await Run(cache);
        Assert.Equal("HIT", ctx.Response.Headers["X-PageSpeed"].ToString());
    }

    [Fact]
    public async Task Miss_OffersApplicationHeaders_WithoutTheMiddlewaresOwnLines()
    {
        var cache = new FakeCache { EarlyHints = Encoding.UTF8.GetBytes("/hint.css\n") };
        await Run(cache, NewContext(), c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers["Content-Security-Policy"] = "default-src 'self'";
            c.Response.Headers.Append("Link", "</app.css>; rel=preload; as=style");
            return c.Response.WriteAsync("body{}");
        });
        var offered = Assert.Single(cache.Offered);
        Assert.Contains(offered, h => h.Key == "Content-Security-Policy");
        var links = offered.Where(h => h.Key == "Link").Select(h => h.Value).ToArray();
        Assert.Equal(new[] { "</app.css>; rel=preload; as=style" }, links);
        Assert.DoesNotContain(offered, h => h.Key == "X-PageSpeed");
    }

    [Fact]
    public async Task Miss_KeepsApplicationVary()
    {
        var cache = new FakeCache();
        var ctx = await Run(cache, NewContext(), c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Vary = "Origin";
            return c.Response.WriteAsync("body{}");
        });
        var vary = ctx.Response.Headers.Vary.ToString();
        Assert.Contains("Origin", vary);
        Assert.Contains("Accept-Encoding", vary);
    }

    [Theory]
    [InlineData("User-Agent")]
    [InlineData("Accept")]
    [InlineData("Save-Data")]
    [InlineData("Origin")]
    [InlineData("Accept-Encoding, User-Agent")]
    public async Task Miss_ApplicationVaryOnARequestHeader_IsNotCached(string vary)
    {
        var cache = new FakeCache();
        var ctx = await Run(cache, NewContext(), c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Vary = vary;
            return c.Response.WriteAsync("body{}");
        });
        Assert.Equal(0, cache.BeginWriteCalls);
        Assert.Equal("MISS", ctx.Response.Headers["X-PageSpeed"].ToString());
    }

    [Theory]
    [InlineData("User-Agent")]
    [InlineData("Accept")]
    [InlineData("Origin")]
    public async Task Miss_VarySetBeforeTheMiddlewareRan_IsNotCached(string vary)
    {
        // A token already on the response when the middleware is entered
        // (set by middleware ahead of it) is not stored with the entry, but
        // the response still varies on it, so it may not be shared.
        var cache = new FakeCache();
        var ctx = NewContext();
        ctx.Response.Headers.Vary = vary;
        await Run(cache, ctx, c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            return c.Response.WriteAsync("body{}");
        });
        Assert.Equal(0, cache.BeginWriteCalls);
        Assert.Contains(vary, ctx.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task Miss_VaryOnAcceptEncodingOnly_IsStillCached()
    {
        var cache = new FakeCache();
        var ctx = NewContext();
        ctx.Response.Headers.Vary = "Accept-Encoding";
        await Run(cache, ctx, c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Append("Vary", "accept-encoding");
            return c.Response.WriteAsync("body{}");
        });
        Assert.Equal(1, cache.BeginWriteCalls);
    }

    [Fact]
    public async Task Miss_HeadersThatCannotBeReplayed_AreNotCached()
    {
        var cache = new FakeCache { Verdict = HeadersSidecarVerdict.FallThrough, Stored = false };
        var ctx = await Run(cache, NewContext(), c =>
        {
            c.Response.StatusCode = 200;
            c.Response.ContentType = "text/css";
            c.Response.Headers["X-Request-Id"] = "abc";
            return c.Response.WriteAsync("body{}");
        });
        Assert.Single(cache.Offered);
        Assert.Equal(0, cache.BeginWriteCalls);
        Assert.Equal("abc", ctx.Response.Headers["X-Request-Id"].ToString());
    }

    [Fact]
    public void MergeVary_UnionsTokensCaseInsensitively_AndKeepsStar()
    {
        IHeaderDictionary h = new HeaderDictionary { ["Vary"] = "Origin, accept-encoding" };
        PageSpeedMiddleware.MergeVary(h, "Accept-Encoding, User-Agent");
        Assert.Equal("Origin, accept-encoding, User-Agent", h.Vary.ToString());
        PageSpeedMiddleware.MergeVary(h, "*");
        Assert.Equal("*", h.Vary.ToString());
    }

    [Fact]
    public void HeadersSidecar_TryParse_RejectsTruncatedAndUnknownVersion()
    {
        var good = EncodeBlock(("X-A", "b"));
        Assert.True(HeadersSidecar.TryParse(good, out var fields));
        Assert.Equal("X-A", fields[0].Key);
        Assert.False(HeadersSidecar.TryParse(good.AsSpan(0, good.Length - 1), out _));
        var bad = (byte[])good.Clone();
        bad[0] = 9;
        Assert.False(HeadersSidecar.TryParse(bad, out _));
        Assert.False(HeadersSidecar.TryParse(ReadOnlySpan<byte>.Empty, out _));
    }
}

/// <summary>
/// The same behaviour against the real native cache and a test server. These
/// need the native library: they fail, not skip, when it is missing or too old.
/// </summary>
public class ResponseHeaderReplayNativeTests : IDisposable
{
    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_hdr_{Guid.NewGuid():N}");
    private readonly PageSpeedCache _cache;

    public ResponseHeaderReplayNativeTests()
    {
        Directory.CreateDirectory(_dir);
        _cache = new PageSpeedCache(new WeAmp.PageSpeed.PageSpeedCacheOptions
        {
            VolumePath = Path.Combine(_dir, "volume.dat"),
            VolumeSizeBytes = 16 * 1024 * 1024,
        });
    }

    public void Dispose()
    {
        _cache.Dispose();
        try { Directory.Delete(_dir, true); } catch { }
    }

    private static PageSpeedMiddleware CreateReal(IPageSpeedCache cache, RequestDelegate next)
    {
        var options = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        options.CurrentValue.Returns(new PageSpeedOptions { Enabled = true });
        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        var notifier = new WorkerNotificationService(
            notifierOptions,
            new WeAmp.PageSpeed.AspNetCore.Internal.InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());
        return new PageSpeedMiddleware(
            next, options, Substitute.For<IHtmlProcessor>(), cache, notifier,
            Substitute.For<ILogger<PageSpeedMiddleware>>());
    }

    private async Task<List<(int Status, HttpResponseMessage Response, string Body)>> Get(
        Func<HttpContext, Func<Task>, Task>? outer, RequestDelegate app, string path,
        int times = 2, Action<HttpRequestMessage>? configure = null)
    {
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseTestServer();
        await using var web = builder.Build();
        if (outer != null) web.Use(outer);
        web.Use(next => CreateReal(_cache, next).InvokeAsync);
        web.Run(app);
        await web.StartAsync();
        var client = web.GetTestClient();
        var results = new List<(int, HttpResponseMessage, string)>();
        for (int i = 0; i < times; i++)
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, path);
            configure?.Invoke(req);
            var r = await client.SendAsync(req);
            results.Add(((int)r.StatusCode, r, await r.Content.ReadAsStringAsync()));
        }
        await web.StopAsync();
        return results;
    }

    private static IEnumerable<string> Values(HttpResponseMessage r, string name) =>
        r.Headers.TryGetValues(name, out var v) ? v
        : r.Content.Headers.TryGetValues(name, out var c) ? c
        : Enumerable.Empty<string>();

    [Fact]
    public async Task HitAnd304CarryApplicationHeaders()
    {
        int runs = 0;
        var res = await Get(null, c =>
        {
            runs++;
            c.Response.ContentType = "text/css";
            c.Response.Headers["Content-Security-Policy"] = "default-src 'self'";
            c.Response.Headers["X-Content-Type-Options"] = "nosniff";
            c.Response.Headers["Access-Control-Allow-Origin"] = "https://app.example";
            // The one request header a stored response may vary on, spelled
            // as the application spelled it so the replayed line is its own.
            c.Response.Headers.Vary = "accept-encoding";
            return c.Response.WriteAsync("body { color: red }");
        }, "/a.css");
        Assert.Equal(1, runs);
        var hit = res[1].Response;
        Assert.Equal("HIT", Values(hit, "X-PageSpeed").Single());
        Assert.Equal("default-src 'self'", Values(hit, "Content-Security-Policy").Single());
        Assert.Equal("nosniff", Values(hit, "X-Content-Type-Options").Single());
        Assert.Equal("https://app.example", Values(hit, "Access-Control-Allow-Origin").Single());
        var vary = string.Join(",", Values(hit, "Vary"));
        Assert.Contains("accept-encoding", vary, StringComparison.Ordinal);
        Assert.Equal("body { color: red }", res[1].Body);
        Assert.Equal(res[1].Body.Length, hit.Content.Headers.ContentLength);

        var cond = await Get(null, _ => throw new InvalidOperationException("reached app"),
            "/a.css", 1, r => r.Headers.IfNoneMatch.ParseAdd(Values(hit, "ETag").Single()));
        Assert.Equal(304, cond[0].Status);
        Assert.Equal("default-src 'self'", Values(cond[0].Response, "Content-Security-Policy").Single());
        Assert.Equal("nosniff", Values(cond[0].Response, "X-Content-Type-Options").Single());
    }

    [Theory]
    [InlineData("User-Agent", "User-Agent")]
    [InlineData("Accept", "Accept")]
    [InlineData("Save-Data", "Save-Data")]
    [InlineData("Origin", "Origin")]
    public async Task AppVaryOnARequestHeader_EachRequestGetsItsOwnAnswer(string vary, string header)
    {
        int runs = 0;
        var res = await Get(null, c =>
        {
            runs++;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Vary = vary;
            return c.Response.WriteAsync($"/* {c.Request.Headers[header]} {runs} */");
        }, "/v-" + vary + ".css", 3);
        Assert.Equal(3, runs);
        Assert.All(res, r => Assert.NotEqual("HIT", Values(r.Response, "X-PageSpeed").SingleOrDefault()));
    }

    [Fact]
    public async Task OuterMiddlewareVary_KeepsTheResponseOutOfTheCache()
    {
        int runs = 0;
        var res = await Get(
            (c, next) => { c.Response.Headers.Vary = "Origin"; return next(); },
            c => { runs++; c.Response.ContentType = "text/css"; return c.Response.WriteAsync("o{}"); },
            "/outer-vary.css", 3);
        Assert.Equal(3, runs);
        Assert.All(res, r => Assert.NotEqual("HIT", Values(r.Response, "X-PageSpeed").SingleOrDefault()));
    }

    [Fact]
    public async Task OuterMiddlewareVaryOnAcceptEncoding_IsStillCached()
    {
        int runs = 0;
        var res = await Get(
            (c, next) => { c.Response.Headers.Vary = "Accept-Encoding"; return next(); },
            c => { runs++; c.Response.ContentType = "text/css"; return c.Response.WriteAsync("ae{}"); },
            "/outer-ae.css");
        Assert.Equal(1, runs);
        Assert.Equal("HIT", Values(res[1].Response, "X-PageSpeed").Single());
    }

    [Fact]
    public void VaryStoreRule_GivesTheNativeStorePredicatesAnswers()
    {
        string?[] values =
        {
            null, "", " ", ",", "Accept-Encoding", "accept-encoding", " Accept-Encoding ,\taccept-encoding",
            "*", "Accept-Encoding, *", "Accept", "User-Agent", "Save-Data", "Origin", "Cookie",
            "Accept-Encoding, User-Agent", "Accept-Encodings", "Accept-", "Sec-CH-DPR",
        };
        foreach (var v in values)
        {
            bool native = WeAmp.PageSpeed.Native.NativePageSpeed.VaryUncacheable(v) != 0;
            bool managed = WeAmp.PageSpeed.AspNetCore.Internal.VaryStoreRule.Refuses(new[] { v });
            Assert.True(native == managed, $"Vary [{v}]: native {native}, managed {managed}");
        }
    }

    [Fact]
    public async Task HitETag_IsDerivedFromBodyNotFromTheApplicationValue()
    {
        int k = 0;
        var res = await Get(null, c =>
        {
            c.Response.ContentType = "text/css";
            c.Response.Headers.ETag = $"\"app-{++k}\"";
            return c.Response.WriteAsync("body { color: red }");
        }, "/etag.css");
        var hit = res[1].Response;
        Assert.Equal("HIT", Values(hit, "X-PageSpeed").Single());
        var expected = PageSpeedMiddleware.ComputeContentETag(
            Encoding.UTF8.GetBytes("body { color: red }"));
        Assert.Equal(expected, Values(hit, "ETag").Single());
    }

    [Fact]
    public async Task OuterMiddlewareHeader_IsNotStoredOrReplayed()
    {
        int runs = 0, k = 0;
        var res = await Get(
            (c, next) => { c.Response.Headers["Access-Control-Allow-Origin"] = $"https://o{++k}.example"; return next(); },
            c => { runs++; c.Response.ContentType = "text/css"; return c.Response.WriteAsync("z{}"); },
            "/outer.css");
        Assert.Equal(1, runs);
        Assert.Equal("HIT", Values(res[1].Response, "X-PageSpeed").Single());
        Assert.Equal("https://o2.example", Values(res[1].Response, "Access-Control-Allow-Origin").Single());
    }

    [Fact]
    public async Task OuterRequestIdHeader_DoesNotBlockCaching()
    {
        int runs = 0, rid = 0;
        var res = await Get(
            (c, next) => { c.Response.Headers["X-Request-Id"] = $"r{++rid}"; return next(); },
            c => { runs++; c.Response.ContentType = "text/css"; return c.Response.WriteAsync("y{}"); },
            "/rid.css");
        Assert.Equal(1, runs);
        Assert.Equal("HIT", Values(res[1].Response, "X-PageSpeed").Single());
        Assert.Equal("r2", Values(res[1].Response, "X-Request-Id").Single());
    }

    [Fact]
    public async Task AppHeaderReplacesOuterHeaderOfTheSameName_OnHit()
    {
        int runs = 0, k = 0;
        var res = await Get(
            (c, next) => { c.Response.Headers["Referrer-Policy"] = $"origin-{++k}"; return next(); },
            c =>
            {
                runs++;
                c.Response.ContentType = "text/css";
                c.Response.Headers["Referrer-Policy"] = "no-referrer";
                return c.Response.WriteAsync("w{}");
            },
            "/repl.css");
        Assert.Equal(1, runs);
        Assert.Equal("no-referrer", Values(res[1].Response, "Referrer-Policy").Single());
    }

    [Fact]
    public async Task CookieAddedWhenTheResponseStarts_IsNeverSharedBetweenUsers()
    {
        int runs = 0;
        var res = await Get(null, c =>
        {
            runs++;
            var me = $"user-{runs}";
            c.Response.OnStarting(() => { c.Response.Headers.Append("Set-Cookie", $"session={me}"); return Task.CompletedTask; });
            c.Response.ContentType = "text/css";
            return c.Response.WriteAsync($"/* hello {me} */");
        }, "/cookie.css");
        Assert.Equal(2, runs);
        Assert.Equal("session=user-1", Values(res[0].Response, "Set-Cookie").Single());
        Assert.Equal("session=user-2", Values(res[1].Response, "Set-Cookie").Single());
        Assert.Equal("/* hello user-2 */", res[1].Body);
    }

    [Fact]
    public async Task CookieAddedWhenTheResponseStartsByOuterMiddleware_IsNeverSharedBetweenUsers()
    {
        int runs = 0, users = 0;
        var res = await Get(
            (c, next) =>
            {
                var me = $"user-{++users}";
                c.Response.OnStarting(() => { c.Response.Headers.Append("Set-Cookie", $"session={me}"); return Task.CompletedTask; });
                return next();
            },
            c => { runs++; c.Response.ContentType = "text/css"; return c.Response.WriteAsync($"/* body {runs} */"); },
            "/outer-cookie.css");
        Assert.Equal(2, runs);
        Assert.Equal("session=user-1", Values(res[0].Response, "Set-Cookie").Single());
        Assert.Equal("session=user-2", Values(res[1].Response, "Set-Cookie").Single());
        Assert.Equal("/* body 2 */", res[1].Body);
    }

    [Fact]
    public async Task HeaderAddedWhenTheResponseStarts_IsReplayedOnHit()
    {
        int runs = 0;
        var res = await Get(null, c =>
        {
            runs++;
            c.Response.OnStarting(() => { c.Response.Headers["Content-Security-Policy"] = "default-src 'self'"; return Task.CompletedTask; });
            c.Response.ContentType = "text/css";
            return c.Response.WriteAsync("x{}");
        }, "/start.css");
        Assert.Equal(1, runs);
        Assert.Equal("HIT", Values(res[1].Response, "X-PageSpeed").Single());
        Assert.Equal("default-src 'self'", Values(res[1].Response, "Content-Security-Policy").Single());
    }

    [Fact]
    public async Task ResponseWithCookie_IsNeverCached_AndEachUserGetsTheirOwn()
    {
        int runs = 0;
        var res = await Get(null, c =>
        {
            runs++;
            c.Response.ContentType = "text/css";
            c.Response.Headers.Append("Set-Cookie", $"session=user-{runs}");
            return c.Response.WriteAsync("per user");
        }, "/me.css");
        Assert.Equal(2, runs);
        Assert.Equal("session=user-2", Values(res[1].Response, "Set-Cookie").Single());
    }

    [Fact]
    public async Task VaryOrigin_AndRequestSpecificHeaders_AreNotCached()
    {
        int originRuns = 0;
        var o = await Get(null, c =>
        {
            originRuns++;
            c.Response.ContentType = "text/css";
            c.Response.Headers["Access-Control-Allow-Origin"] = "https://app.example";
            c.Response.Headers.Vary = "Origin";
            return c.Response.WriteAsync("x");
        }, "/o.css");
        Assert.Equal(2, originRuns);
        Assert.Contains("Origin", string.Join(",", Values(o[1].Response, "Vary")));

        int idRuns = 0;
        var i = await Get(null, c =>
        {
            idRuns++;
            c.Response.ContentType = "text/css";
            c.Response.Headers["X-Request-Id"] = $"r{idRuns}";
            return c.Response.WriteAsync("x");
        }, "/id.css");
        Assert.Equal(2, idRuns);
        Assert.Equal("r2", Values(i[1].Response, "X-Request-Id").Single());
    }

    [Fact]
    public void Gate_RefusesCookiesAndNonceBearingPolicies_AndStoresNothing()
    {
        KeyValuePair<string, string> H(string n, string v) => new(n, v);

        var cookie = _cache.WriteHeadersSidecar("/g1", "h", "https", new[]
        {
            H("Content-Type", "text/css"), H("Set-Cookie", "session=a"),
        });
        Assert.Equal(HeadersSidecarVerdict.FallThrough, cookie.Verdict);
        Assert.False(cookie.Stored);
        Assert.Null(_cache.ReadAlternate("/g1", "h", "https", SentinelId.HeadersSidecar));

        var nonce = _cache.WriteHeadersSidecar("/g2", "h", "https", new[]
        {
            H("Content-Type", "text/css"),
            H("Content-Security-Policy", "script-src 'nonce-abc123'"),
        });
        Assert.Equal(HeadersSidecarVerdict.NeverOptimized, nonce.Verdict);
        Assert.False(nonce.Stored);
        Assert.Null(_cache.ReadAlternate("/g2", "h", "https", SentinelId.HeadersSidecar));

        var ok = _cache.WriteHeadersSidecar("/g3", "h", "https", new[]
        {
            H("Content-Type", "text/css"), H("X-Content-Type-Options", "nosniff"),
        });
        Assert.Equal(HeadersSidecarVerdict.SwapEligible, ok.Verdict);
        Assert.True(ok.Stored);
        using var block = _cache.ReadAlternate("/g3", "h", "https", SentinelId.HeadersSidecar);
        Assert.NotNull(block);
        Assert.True(HeadersSidecar.TryParse(block!.ContentMemory.Span, out var fields));
        Assert.Equal(new[] { "X-Content-Type-Options" }, fields.Select(f => f.Key));
    }
}

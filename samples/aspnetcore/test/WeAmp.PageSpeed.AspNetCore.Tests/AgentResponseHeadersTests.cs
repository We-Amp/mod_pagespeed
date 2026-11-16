// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Net.Http;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Microsoft.Extensions.Primitives;
using NSubstitute;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.Native;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The response headers agent optimization adds, matching the nginx module:
/// <c>Vary: Accept</c> on every HTML response while a Markdown copy may be
/// served at the URL, the <c>x-markdown-tokens</c> estimate and an
/// operator-set <c>content-signal</c> on the Markdown copy. The added
/// <c>Accept</c> goes on the outgoing response only: it never reaches the
/// stored header block or the store-side "origin varies on Accept" mark.
/// The Markdown copy itself is covered in
/// <see cref="MarkdownContentNegotiationTests"/>.
/// </summary>
public class AgentResponseHeadersTests
{
    // ── token estimate and content-signal validation (pure) ──────────

    [Theory]
    [InlineData(0L, 0L)]
    [InlineData(3L, 0L)]
    [InlineData(4L, 1L)]
    [InlineData(7L, 1L)]
    [InlineData(8L, 2L)]
    [InlineData(4099L, 1024L)]
    [InlineData(-5L, 0L)]
    public void TokenEstimate_IsBodyBytesOverFourRoundedDown(long bytes, long tokens)
    {
        Assert.Equal(tokens, AgentMarkdownHeaders.EstimateTokens(bytes));
    }

    [Theory]
    [InlineData("search=yes, ai-input=yes, ai-train=no")]
    [InlineData("ai-train=no")]
    [InlineData("a\tb")]
    [InlineData("x")]
    public void ContentSignal_Accepted(string value)
    {
        Assert.True(AgentMarkdownHeaders.IsValidContentSignalValue(value));
    }

    [Theory]
    [InlineData("")]
    [InlineData("ai-train=no\r\nSet-Cookie: a=b")]
    [InlineData("ai-train=no\nX: y")]
    [InlineData("ai-train=no\r")]
    [InlineData("a\0b")]
    [InlineData("a\u0001b")]
    [InlineData("a\u007Fb")]
    [InlineData("ai-train=n\u00F6")]
    [InlineData("ai-train=\u2028no")]
    [InlineData(" ai-train=no")]
    [InlineData("ai-train=no ")]
    [InlineData("\tai-train=no")]
    public void ContentSignal_Refused(string value)
    {
        Assert.False(AgentMarkdownHeaders.IsValidContentSignalValue(value));
    }

    [Fact]
    public void ContentSignal_SizeBound_Is512()
    {
        Assert.True(AgentMarkdownHeaders.IsValidContentSignalValue(new string('a', 512)));
        Assert.False(AgentMarkdownHeaders.IsValidContentSignalValue(new string('a', 513)));
        Assert.False(AgentMarkdownHeaders.IsValidContentSignalValue(null));
    }

    [Fact]
    public void Add_SendsNoContentSignal_ForAnInvalidOrMissingValue()
    {
        var headers = new HeaderDictionary();
        AgentMarkdownHeaders.Add(headers, 10, "a\r\nb: c");
        Assert.Equal("2", headers["x-markdown-tokens"].ToString());
        Assert.False(headers.ContainsKey("content-signal"));

        headers = new HeaderDictionary();
        AgentMarkdownHeaders.Add(headers, 10, null);
        Assert.False(headers.ContainsKey("content-signal"));

        headers = new HeaderDictionary();
        AgentMarkdownHeaders.Add(headers, 10, "ai-train=no");
        Assert.Equal("ai-train=no", headers["content-signal"].ToString());
    }

    [Theory]
    [InlineData(null, true)]
    [InlineData("", true)]
    [InlineData("search=yes, ai-train=no", true)]
    [InlineData("ai-train=no\r\nX-Injected: 1", false)]
    [InlineData("ai-train=\u00E9", false)]
    [InlineData(" lead", false)]
    public void Validator_ChecksAgentContentSignal(string? value, bool ok)
    {
        var result = new PageSpeedOptionsValidator().Validate(
            null, new PageSpeedOptions { AgentContentSignal = value });
        Assert.Equal(ok, result.Succeeded);
        if (!ok) Assert.Contains("AgentContentSignal", result.FailureMessage);
    }

    [Fact]
    public void Validator_RefusesAnOverlongContentSignal()
    {
        var result = new PageSpeedOptionsValidator().Validate(
            null, new PageSpeedOptions { AgentContentSignal = new string('a', 513) });
        Assert.False(result.Succeeded);
    }

    // ── Vary: Accept on an HTML cache hit ────────────────────────────

    private static readonly byte[] Html = Encoding.UTF8.GetBytes("<html><body>hi</body></html>");

    private const byte Recorded = CacheFlags.HeadersClassified | CacheFlags.AnonymousRecorded;

    [Theory]
    // agent optimize, worker entitlement, expected Vary
    [InlineData(false, 1, "Accept-Encoding, User-Agent")]
    [InlineData(true, 0, "Accept-Encoding, User-Agent")]
    [InlineData(true, 1, "Accept-Encoding, User-Agent, Accept")]
    public async Task HtmlHit_NamesAccept_OnlyWhileAgentOptimizationIsOn(
        bool agentOptimize, int entitled, string expectedVary)
    {
        var cache = new FakeCache
        {
            Entitled = entitled,
            Hit = new Result { ContentMemory = Html, Flags = Recorded },
        };
        var ctx = await Run(cache, new PageSpeedOptions
        {
            Enabled = true, EnableAgentOptimize = agentOptimize,
        }, accept: "text/html");

        Assert.Equal("HIT", ctx.Response.Headers["X-PageSpeed"].ToString());
        Assert.Equal(expectedVary, ctx.Response.Headers.Vary.ToString());
        Assert.False(ctx.Response.Headers.ContainsKey("x-markdown-tokens"));
        Assert.False(ctx.Response.Headers.ContainsKey("content-signal"));
    }

    [Fact]
    public async Task HtmlHit_UnreadableWorkerSetting_NamesAccept()
    {
        var cache = new FakeCache
        {
            EntitledThrows = true,
            Hit = new Result { ContentMemory = Html, Flags = Recorded },
        };
        var ctx = await Run(cache, new PageSpeedOptions
        {
            Enabled = true, EnableAgentOptimize = true,
        }, accept: "text/html");

        Assert.Equal("Accept-Encoding, User-Agent, Accept", ctx.Response.Headers.Vary.ToString());
    }

    [Fact]
    public async Task HtmlHit_OriginAlreadyVariesOnAccept_ListsItOnce()
    {
        var cache = new FakeCache
        {
            Entitled = 1,
            Hit = new Result
            {
                ContentMemory = Html,
                Flags = Recorded | CacheFlags.HeadersSidecarRequired | CacheFlags.OriginVariesAccept,
            },
            Block = EncodeBlock(("Vary", "Accept")),
        };
        var ctx = await Run(cache, new PageSpeedOptions
        {
            Enabled = true, EnableAgentOptimize = true,
        }, accept: "text/html");

        var tokens = Tokens(ctx.Response.Headers.Vary);
        Assert.Single(tokens, t => t.Equals("Accept", StringComparison.OrdinalIgnoreCase));
        Assert.Contains("Accept-Encoding", tokens);
        Assert.Contains("User-Agent", tokens);
    }

    [Fact]
    public async Task CssHit_IsNotAffected()
    {
        var cache = new FakeCache
        {
            Entitled = 1,
            Hit = new Result
            {
                ContentMemory = Encoding.UTF8.GetBytes("a{}"),
                Flags = Recorded,
                ContentType = PageSpeedContentType.Css,
                OriginContentType = "text/css",
            },
        };
        var ctx = await Run(cache, new PageSpeedOptions
        {
            Enabled = true, EnableAgentOptimize = true,
        }, path: "/site.css");

        Assert.Equal("Accept-Encoding", ctx.Response.Headers.Vary.ToString());
    }

    // ── Vary: Accept on an HTML miss: outgoing only, never stored ────
    // These run the real HTML processor (native library) so the miss path
    // stores the page. The response feature raises OnStarting when the body
    // is first written, as a server does.

    private const string Page =
        "<!doctype html><html><head><title>t</title>" +
        "<link rel=\"stylesheet\" href=\"/site.css\">" +
        "</head><body><img src=\"/a.jpg\"><p>hello</p></body></html>";

    [NativeLibTheory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task HtmlMiss_AddedAcceptGoesOutButIsNotStored(bool raiseOnStarting)
    {
        var on = await RunMiss(agentOptimize: true, entitled: 1, raiseOnStarting,
            next: OriginPage(vary: null, lateVary: null));
        var off = await RunMiss(agentOptimize: false, entitled: 1, raiseOnStarting,
            next: OriginPage(vary: null, lateVary: null));

        // Outgoing: Accept is named only with agent optimization on.
        Assert.Contains("Accept", Tokens(on.Context.Response.Headers.Vary));
        Assert.DoesNotContain("Accept", Tokens(off.Context.Response.Headers.Vary));
        Assert.Equal("MISS", on.Context.Response.Headers["X-PageSpeed"].ToString());

        // Stored: what the store decision reads is the same either way.
        Assert.NotNull(on.Cache.WrittenFlags);
        Assert.Equal(off.Cache.WrittenFlags, on.Cache.WrittenFlags);
        Assert.Equal(0, on.Cache.WrittenFlags!.Value & CacheFlags.OriginVariesAccept);
        Assert.Equal(Serialize(off.Cache.Offered), Serialize(on.Cache.Offered));
        Assert.DoesNotContain(on.Cache.Offered.Single(),
            h => h.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase) &&
                 Tokens(h.Value).Contains("Accept"));
    }

    [NativeLibTheory]
    [InlineData("Accept", null)]
    [InlineData(null, "Accept")]
    public async Task HtmlMiss_OriginsOwnAccept_StaysTheOrigins_AndIsListedOnce(
        string? vary, string? lateVary)
    {
        // The origin names Accept itself, in its handler or in an OnStarting
        // callback: it counts as the origin's, so the store rule keeps the
        // response out of the cache exactly as with agent optimization off,
        // and the outgoing response lists Accept once.
        var on = await RunMiss(agentOptimize: true, entitled: 1, raiseOnStarting: true,
            next: OriginPage(vary, lateVary));
        var off = await RunMiss(agentOptimize: false, entitled: 1, raiseOnStarting: true,
            next: OriginPage(vary, lateVary));

        Assert.Null(off.Cache.WrittenFlags);
        Assert.Null(on.Cache.WrittenFlags);
        Assert.Single(Tokens(on.Context.Response.Headers.Vary),
            t => t.Equals("Accept", StringComparison.OrdinalIgnoreCase));
    }

    [NativeLibFact]
    public async Task HtmlMiss_AgentOnButNotEntitled_DoesNotNameAccept()
    {
        var r = await RunMiss(agentOptimize: true, entitled: 0, raiseOnStarting: true,
            next: OriginPage(vary: null, lateVary: null));
        Assert.DoesNotContain("Accept", Tokens(r.Context.Response.Headers.Vary));
    }

    // ── harness ──────────────────────────────────────────────────────

    private static RequestDelegate OriginPage(string? vary, string? lateVary) => async ctx =>
    {
        ctx.Response.StatusCode = 200;
        ctx.Response.ContentType = "text/html; charset=utf-8";
        ctx.Response.Headers.CacheControl = "public, max-age=60";
        if (vary != null) ctx.Response.Headers.Append("Vary", vary);
        if (lateVary != null)
            ctx.Response.OnStarting(() =>
            {
                ctx.Response.Headers.Append("Vary", lateVary);
                return Task.CompletedTask;
            });
        await ctx.Response.Body.WriteAsync(Encoding.UTF8.GetBytes(Page));
    };

    private static async Task<(DefaultHttpContext Context, FakeCache Cache)> RunMiss(
        bool agentOptimize, int entitled, bool raiseOnStarting, RequestDelegate next)
    {
        var options = new PageSpeedOptions { Enabled = true, EnableAgentOptimize = agentOptimize };
        var monitor = Monitor(options);
        var cache = new FakeCache { Entitled = entitled };
        var middleware = new PageSpeedMiddleware(
            next, monitor, new HtmlProcessor(null, monitor), cache, Notifier(),
            Substitute.For<ILogger<PageSpeedMiddleware>>());

        var ctx = NewContext("/page", "text/html");
        if (raiseOnStarting)
        {
            var feature = new StartingResponseFeature();
            ctx.Features.Set<IHttpResponseFeature>(feature);
            ctx.Response.Body = new StartOnWriteStream(feature);
        }
        await middleware.InvokeAsync(ctx);
        return (ctx, cache);
    }

    private static async Task<DefaultHttpContext> Run(
        FakeCache cache, PageSpeedOptions options, string path = "/page",
        string? accept = null)
    {
        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, Monitor(options), Substitute.For<IHtmlProcessor>(),
            cache, Notifier(), Substitute.For<ILogger<PageSpeedMiddleware>>());
        var ctx = NewContext(path, accept);
        await middleware.InvokeAsync(ctx);
        return ctx;
    }

    private static DefaultHttpContext NewContext(string path, string? accept)
    {
        var ctx = new DefaultHttpContext();
        ctx.Request.Method = "GET";
        ctx.Request.Path = path;
        ctx.Request.Scheme = "https";
        ctx.Request.Host = new HostString("example.test");
        if (accept != null) ctx.Request.Headers.Accept = accept;
        ctx.Response.Body = new MemoryStream();
        return ctx;
    }

    private static IOptionsMonitor<PageSpeedOptions> Monitor(PageSpeedOptions options)
    {
        var monitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        monitor.CurrentValue.Returns(options);
        return monitor;
    }

    private static WorkerNotificationService Notifier()
    {
        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        return new WorkerNotificationService(
            notifierOptions, new InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());
    }

    private static List<string> Tokens(StringValues lines) =>
        lines.Where(l => l != null)
             .SelectMany(l => l!.Split(','))
             .Select(t => t.Trim())
             .Where(t => t.Length > 0)
             .ToList();

    private static List<string> Tokens(string line) => Tokens(new StringValues(line));

    private static string Serialize(List<IReadOnlyList<KeyValuePair<string, string>>> offered) =>
        string.Join("\n", offered.Select(set =>
            string.Join("|", set.Select(h => $"{h.Key}={h.Value}"))));

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

    /// <summary>Raises the registered OnStarting callbacks on the first write.</summary>
    private sealed class StartingResponseFeature : HttpResponseFeature
    {
        private readonly List<(Func<object, Task> Callback, object State)> _callbacks = new();
        private bool _started;

        public override bool HasStarted => _started;

        public override void OnStarting(Func<object, Task> callback, object state) =>
            _callbacks.Add((callback, state));

        public async Task StartAsync()
        {
            if (_started) return;
            // Last registered runs first, as in the server.
            for (int i = _callbacks.Count - 1; i >= 0; i--)
                await _callbacks[i].Callback(_callbacks[i].State);
            _started = true;
        }
    }

    private sealed class StartOnWriteStream(StartingResponseFeature feature) : MemoryStream
    {
        public override async ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default)
        {
            await feature.StartAsync();
            await base.WriteAsync(buffer, cancellationToken);
        }

        public override async Task WriteAsync(
            byte[] buffer, int offset, int count, CancellationToken cancellationToken)
        {
            await feature.StartAsync();
            await base.WriteAsync(buffer, offset, count, cancellationToken);
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            feature.StartAsync().GetAwaiter().GetResult();
            base.Write(buffer, offset, count);
        }
    }

    private sealed class Result : IReadResult
    {
        public ReadOnlyMemory<byte> ContentMemory { get; init; }
        public uint Mask { get; init; } = 0x08;
        public PageSpeedContentType ContentType { get; init; } = PageSpeedContentType.Html;
        public string? OriginContentType { get; init; } = "text/html; charset=utf-8";
        public byte Flags { get; init; }
        public bool NeedsRevalidation => false;
        public ushort OriginCcFlags => 0;
        public uint OriginSMaxage => 0;
        public bool RevalidationRequired => false;
        public uint CacheInsertedAt => 1_700_000_000;
        public uint OriginContentLength => 0;
        public bool IsWorkerProcessed => false;
        public bool RenewLease() => false;
        public void Dispose() { }
    }

    private sealed class FakeCache : IPageSpeedCache
    {
        public IReadResult? Hit { get; set; }
        public byte[]? Block { get; set; }
        public int Entitled { get; set; }
        public bool EntitledThrows { get; set; }
        public List<IReadOnlyList<KeyValuePair<string, string>>> Offered { get; } = new();
        public byte? WrittenFlags { get; private set; }

        SafeCacheHandle IPageSpeedCache.Handle => throw new NotSupportedException();

        public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask) => Hit;
        public IReadResult? ReadBestAgent(string url, string hostname, string scheme, uint mask, int e) => Hit;

        public int ReadSharedConfigAgentEntitled() =>
            EntitledThrows ? throw new InvalidOperationException("unreadable") : Entitled;

        public IReadResult? ReadAlternate(string url, string hostname, string scheme, byte id) =>
            id == SentinelId.HeadersSidecar && Block != null
                ? new Result { ContentMemory = Block }
                : null;

        public IReadResult? ReadEarlyHints(string url, string hostname, string scheme) => null;

        public CacheWriter BeginWrite(string url, string hostname, string scheme, CacheWriteParams p)
        {
            // Record what the store decision produced; the write itself is
            // not needed (the middleware logs and carries on).
            WrittenFlags = p.Flags;
            throw new NotSupportedException();
        }

        public CacheWriter BeginWriteSentinel(string url, string hostname, string scheme, byte id, ulong len) =>
            throw new NotSupportedException();

        public HeadersSidecarWriteResult WriteHeadersSidecar(
            string url, string hostname, string scheme,
            IReadOnlyList<KeyValuePair<string, string>> headers)
        {
            Offered.Add(headers.ToList());
            return new(HeadersSidecarVerdict.SwapEligible, true);
        }

        public CacheStats GetStats() => new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        public void RecordServeHit(PageSpeedContentType t, uint o, ulong b, uint m) { }
        public void Dispose() { }
    }
}

/// <summary>
/// Against the real cache (native library): with agent optimization on, a
/// page whose origin sends no <c>Vary</c> goes MISS then HIT, both responses
/// name <c>Accept</c> in <c>Vary</c>, and the stored entry holds no
/// <c>Accept</c> of the middleware's own. A conditional request answered 304
/// from the entry leaves it storable and served as a HIT. (The middleware
/// never sends a conditional request to the application itself; a stale
/// entry is refreshed by the worker, so there is no 304 restamp here.)
/// </summary>
public sealed class AgentVaryStoredEntryTests : IDisposable
{
    private const string Page =
        "<!doctype html><html><head><title>t</title>" +
        "<link rel=\"stylesheet\" href=\"/site.css\">" +
        "</head><body><img src=\"/a.jpg\"><p>hello</p></body></html>";

    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_agentvary_{Guid.NewGuid():N}");

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch { }
    }

    [NativeLibTheory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task NoOriginVary_MissThenHit_StoredEntryHasNoAcceptOfOurs(bool agentOptimize)
    {
        Directory.CreateDirectory(_dir);
        using var real = new PageSpeedCache(new WeAmp.PageSpeed.PageSpeedCacheOptions
        {
            VolumePath = Path.Combine(_dir, "volume.dat"),
            VolumeSizeBytes = 16 * 1024 * 1024,
        });
        var cache = new AgentEntitledCache(real);
        var options = new PageSpeedOptions { Enabled = true, EnableAgentOptimize = agentOptimize };
        var monitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        monitor.CurrentValue.Returns(options);
        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        var notifier = new WorkerNotificationService(
            notifierOptions, new InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());

        // The app's answers to the visitor, and to the middleware's own
        // anonymous request from 224.0.0.0 that a stored response comes from
        // (AnonymousFetchCheck).
        int originRuns = 0;
        int anonymousRuns = 0;
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseTestServer();
        await using var web = builder.Build();
        web.Use(next => new PageSpeedMiddleware(
            next, monitor, new HtmlProcessor(real, monitor), cache, notifier,
            Substitute.For<ILogger<PageSpeedMiddleware>>()).InvokeAsync);
        RequestDelegate origin = async ctx =>
        {
            if (AgentSelfFetch.Label.Equals(ctx.Connection.RemoteIpAddress))
                anonymousRuns++;
            else
                originRuns++;
            ctx.Response.ContentType = "text/html; charset=utf-8";
            ctx.Response.Headers.CacheControl = "public, max-age=60";
            await ctx.Response.WriteAsync(Page);
        };
        web.Run(origin);
        await web.StartAsync();
        var client = web.GetTestClient();

        async Task<HttpResponseMessage> Get(string? ifNoneMatch = null)
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, "/page");
            req.Headers.Accept.ParseAdd("text/html");
            if (ifNoneMatch != null) req.Headers.IfNoneMatch.ParseAdd(ifNoneMatch);
            var r = await client.SendAsync(req);
            await r.Content.ReadAsByteArrayAsync();
            return r;
        }
        static string Vary(HttpResponseMessage r) =>
            string.Join(", ", r.Headers.TryGetValues("Vary", out var v) ? v : Array.Empty<string>());
        static string Header(HttpResponseMessage r, string name) =>
            r.Headers.TryGetValues(name, out var v) ? string.Join(",", v) : "";
        static int AcceptCount(string vary) =>
            vary.Split(',').Count(t => t.Trim().Equals("Accept", StringComparison.OrdinalIgnoreCase));

        var expectedAccept = agentOptimize ? 1 : 0;

        var miss = await Get();
        Assert.Equal(200, (int)miss.StatusCode);
        Assert.Equal("MISS", Header(miss, "X-PageSpeed"));
        Assert.Equal(expectedAccept, AcceptCount(Vary(miss)));

        var hit = await Get();
        Assert.Equal(200, (int)hit.StatusCode);
        Assert.Equal("HIT", Header(hit, "X-PageSpeed"));
        Assert.Equal(expectedAccept, AcceptCount(Vary(hit)));
        Assert.Equal(1, originRuns);
        Assert.Equal(1, anonymousRuns);

        // The stored entry: what the store decision saw and what was written.
        Assert.Single(cache.Offered);
        Assert.DoesNotContain(cache.Offered[0], h =>
            h.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase) && AcceptCount(h.Value) > 0);
        using (var entry = real.ReadBest("/page", "localhost", "http", 0x08))
        {
            Assert.NotNull(entry);
            Assert.Equal(0, entry!.Flags & CacheFlags.OriginVariesAccept);
        }
        using (var block = real.ReadAlternate("/page", "localhost", "http", SentinelId.HeadersSidecar))
        {
            if (block != null &&
                HeadersSidecar.TryParse(block.ContentMemory.Span, out var fields))
            {
                Assert.DoesNotContain(fields, h =>
                    h.Key.Equals("Vary", StringComparison.OrdinalIgnoreCase) &&
                    AcceptCount(h.Value) > 0);
            }
        }

        // A conditional request is answered 304 from the entry, and the entry
        // still serves as a HIT afterwards.
        var etag = Header(hit, "ETag");
        Assert.NotEmpty(etag);
        var notModified = await Get(etag);
        Assert.Equal(304, (int)notModified.StatusCode);
        Assert.Equal(expectedAccept, AcceptCount(Vary(notModified)));
        var again = await Get();
        Assert.Equal("HIT", Header(again, "X-PageSpeed"));
        Assert.Equal(expectedAccept, AcceptCount(Vary(again)));
        Assert.Equal(1, originRuns);
        Assert.Equal(1, anonymousRuns);

        await web.StopAsync();
    }

    /// <summary>The real cache, with the worker's agent optimization on.</summary>
    private sealed class AgentEntitledCache(PageSpeedCache inner) : IPageSpeedCache
    {
        public List<List<KeyValuePair<string, string>>> Offered { get; } = new();

        public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask) =>
            inner.ReadBest(url, hostname, scheme, mask);
        public IReadResult? ReadBestAgent(string url, string hostname, string scheme, uint mask, int e) =>
            inner.ReadBestAgent(url, hostname, scheme, mask, e);
        public int ReadSharedConfigAgentEntitled() => 1;
        public IReadResult? ReadAlternate(string url, string hostname, string scheme, byte id) =>
            inner.ReadAlternate(url, hostname, scheme, id);
        public IReadResult? ReadEarlyHints(string url, string hostname, string scheme) =>
            inner.ReadEarlyHints(url, hostname, scheme);
        public CacheWriter BeginWrite(string url, string hostname, string scheme, CacheWriteParams p) =>
            inner.BeginWrite(url, hostname, scheme, p);
        public CacheWriter BeginWriteSentinel(string url, string hostname, string scheme, byte id, ulong len) =>
            inner.BeginWriteSentinel(url, hostname, scheme, id, len);
        public CacheStats GetStats() => inner.GetStats();
        public HeadersSidecarWriteResult WriteHeadersSidecar(
            string url, string hostname, string scheme,
            IReadOnlyList<KeyValuePair<string, string>> headers)
        {
            Offered.Add(headers.ToList());
            return inner.WriteHeadersSidecar(url, hostname, scheme, headers);
        }
        public void RecordServeHit(PageSpeedContentType t, uint o, ulong b, uint m) =>
            inner.RecordServeHit(t, o, b, m);
        SafeCacheHandle IPageSpeedCache.Handle => ((IPageSpeedCache)inner).Handle;
        public void Dispose() { }
    }
}

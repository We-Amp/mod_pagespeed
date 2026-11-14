// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net.Http;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.AspNetCore.HostFiltering;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NSubstitute;
using WeAmp.PageSpeed.Native;
using Xunit;
using Xunit.Abstractions;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Proves the .NET middleware serves rendered-DOM markdown to an
/// entitled <c>Accept: text/markdown</c> request at the SAME URL (with
/// <c>Vary: Accept</c>, <c>X-Robots-Tag: noindex</c>, <c>Cache-Control:
/// private, no-cache</c>), while a browser / non-entitled / non-markdown /
/// flag-off request gets unchanged HTML. The entitlement + content-hash binding
/// gate lives entirely in the native <c>ps_cache_read_best_agent</c>; this test
/// drives the managed negotiation/serve logic with a fake cache.
///
/// The negotiation's only native dependency is <c>ps_wants_agent_markdown</c>
/// (the Accept parser). The flag-off kill-switch path short-circuits before
/// that native call (C# &amp;&amp;), so it always runs. The other cases require
/// libpagespeed to be loadable; they SKIP gracefully otherwise (the helper
/// copies a Bazel-built lib into the test's runtimes/&lt;rid&gt;/native layout
/// when present).
/// </summary>
public sealed class MarkdownContentNegotiationTests
{
    private static readonly byte[] HtmlBody =
        Encoding.UTF8.GetBytes("<html><body>hello</body></html>");
    private static readonly byte[] MarkdownBody =
        Encoding.UTF8.GetBytes("# hello\n");

    private readonly ITestOutputHelper _output;

    public MarkdownContentNegotiationTests(ITestOutputHelper output)
    {
        _output = output;
    }

    // ── always-run: the OFF-by-default kill switch (no native call) ──

    [Fact]
    public async Task FlagOff_ServesHtml_EvenForEntitledMarkdownRequest()
    {
        // EnableAgentOptimize=false short-circuits before WantsAgentMarkdown, so
        // even a markdown-capable, entitled cache serves HTML. No native lib
        // needed — this is the load-bearing default-OFF proof.
        var cache = new MarkdownNegotiationCache(agentEntitled: 1);
        var (status, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: false, accept: "text/markdown");

        Assert.Equal(200, status);
        Assert.Equal("text/html; charset=utf-8", ct);
        Assert.Equal(HtmlBody, body);
        Assert.False(headers.ContainsKey("X-Robots-Tag"));
    }

    // ── native-gated: full negotiation matrix (flag ON) ──

    [Fact]
    public async Task EntitledMarkdownRequest_ServesMarkdownAtSameUrl()
    {
        if (!NativeMarkdownProbe.Available(_output)) return;

        var cache = new MarkdownNegotiationCache(agentEntitled: 1);
        var (status, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: true, accept: "text/markdown");

        Assert.Equal(200, status);
        Assert.Equal("text/markdown; charset=utf-8", ct);
        Assert.Equal(MarkdownBody, body);
        Assert.Contains("Accept", headers["Vary"].ToString(),
            StringComparison.OrdinalIgnoreCase);
        Assert.Equal("private, no-cache", headers.CacheControl.ToString());
        Assert.Equal("noindex", headers["X-Robots-Tag"].ToString());
    }

    [Fact]
    public async Task BrowserHtmlAccept_ServesHtml()
    {
        if (!NativeMarkdownProbe.Available(_output)) return;

        var cache = new MarkdownNegotiationCache(agentEntitled: 1);
        var (status, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: true, accept: "text/html");

        Assert.Equal(200, status);
        Assert.Equal("text/html; charset=utf-8", ct);
        Assert.Equal(HtmlBody, body);
        Assert.False(headers.ContainsKey("X-Robots-Tag"));
    }

    [Fact]
    public async Task NotEntitledMarkdownRequest_ServesHtml()
    {
        if (!NativeMarkdownProbe.Available(_output)) return;

        // The worker-written entitlement is 0, so the middleware never routes
        // through the agent gate even though the request wants markdown.
        var cache = new MarkdownNegotiationCache(agentEntitled: 0);
        var (status, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: true, accept: "text/markdown");

        Assert.Equal(200, status);
        Assert.Equal("text/html; charset=utf-8", ct);
        Assert.Equal(HtmlBody, body);
        Assert.False(headers.ContainsKey("X-Robots-Tag"));
    }

    [Fact]
    public async Task WildcardAccept_ServesHtml()
    {
        if (!NativeMarkdownProbe.Available(_output)) return;

        // A wildcard accept must NOT opt into markdown.
        var cache = new MarkdownNegotiationCache(agentEntitled: 1);
        var (status, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: true, accept: "*/*");

        Assert.Equal(200, status);
        Assert.Equal("text/html; charset=utf-8", ct);
        Assert.Equal(HtmlBody, body);
        Assert.False(headers.ContainsKey("X-Robots-Tag"));
    }

    [Fact]
    public async Task EntitledMarkdownRequest_HostNotInAllowedHosts_ServesHtml()
    {
        if (!NativeMarkdownProbe.Available(_output)) return;

        var cache = new MarkdownNegotiationCache(agentEntitled: 1);
        var (status, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: true, accept: "text/markdown",
            allowedHost: "*");

        Assert.Equal(200, status);
        Assert.Equal("text/html; charset=utf-8", ct);
        Assert.Equal(HtmlBody, body);
        Assert.False(headers.ContainsKey("X-Robots-Tag"));
    }

    // ── a request carrying a cookie ───────────────────────────────────

    // The markdown copy has no Cache-Control of its own: for a request with a
    // cookie, the recorded page's mark decides (public, or one s-maxage
    // above zero), and without a recorded page it is not served.
    [NativeLibTheory]
    [InlineData(null, 0, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.MustRevalidate, 0, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.SMaxagePresent, 0, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.SMaxagePresent
                | CacheControlCcFlags.SMaxageRepeated, 60, false)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.Public, 0, true)]
    [InlineData(CacheControlCcFlags.HeaderPresent | CacheControlCcFlags.SMaxagePresent, 60, true)]
    public async Task EntitledMarkdownRequestWithCookie_ServedOnlyWhenTheRecordedPageIsMarkedShared(
        int? pageOriginCcFlags, int pageOriginSMaxage, bool expectMarkdown)
    {
        var cache = new MarkdownNegotiationCache(agentEntitled: 1)
        {
            RecordedPage = pageOriginCcFlags is int f
                ? new NegotiationReadResult
                {
                    ContentMemory = HtmlBody,
                    Mask = 0x08,
                    ContentType = PageSpeedContentType.Html,
                    OriginContentType = "text/html; charset=utf-8",
                    OriginCcFlags = (ushort)f,
                    OriginSMaxage = (uint)pageOriginSMaxage,
                }
                : null,
        };
        var (_, ct, body, headers) = await Serve(
            cache, enableAgentOptimize: true, accept: "text/markdown",
            cookie: "session=abc");

        if (expectMarkdown)
        {
            Assert.Equal("text/markdown; charset=utf-8", ct);
            Assert.Equal(MarkdownBody, body);
        }
        else
        {
            Assert.NotEqual("text/markdown; charset=utf-8", ct);
            Assert.NotEqual(MarkdownBody, body);
            Assert.NotEqual("HIT", headers["X-PageSpeed"].ToString());
        }
    }

    // Control for the theory above: without a cookie the same request is
    // served the markdown copy whatever the recorded page says.
    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task EntitledMarkdownRequestWithoutCookie_ServedWhateverTheRecordedPageSays(
        bool recordedPage)
    {
        var cache = new MarkdownNegotiationCache(agentEntitled: 1)
        {
            RecordedPage = recordedPage
                ? new NegotiationReadResult
                {
                    ContentMemory = HtmlBody,
                    Mask = 0x08,
                    ContentType = PageSpeedContentType.Html,
                    OriginContentType = "text/html; charset=utf-8",
                    OriginCcFlags = CacheControlCcFlags.HeaderPresent,
                }
                : null,
        };
        var (_, ct, body, _) = await Serve(
            cache, enableAgentOptimize: true, accept: "text/markdown");

        Assert.Equal("text/markdown; charset=utf-8", ct);
        Assert.Equal(MarkdownBody, body);
    }

    // ── harness ──────────────────────────────────────────────────────

    private async Task<(int Status, string? ContentType, byte[] Body, IHeaderDictionary Headers)>
        Serve(IPageSpeedCache cache, bool enableAgentOptimize, string accept,
              string allowedHost = "localhost", string? cookie = null)
    {
        var options = new PageSpeedOptions
        {
            Enabled = true,
            EnableAgentOptimize = enableAgentOptimize,
        };
        var optionsMonitor = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        optionsMonitor.CurrentValue.Returns(options);

        var notifierOptions = Substitute.For<IOptionsMonitor<PageSpeedOptions>>();
        notifierOptions.CurrentValue.Returns(new PageSpeedOptions());
        var notifier = new WorkerNotificationService(
            notifierOptions,
            new WeAmp.PageSpeed.AspNetCore.Internal.InternalWorkerEndpoint(),
            Substitute.For<ILogger<WorkerNotificationService>>());

        var hostFiltering = Substitute.For<IOptionsMonitor<HostFilteringOptions>>();
        hostFiltering.CurrentValue.Returns(new HostFilteringOptions
        {
            AllowedHosts = new List<string> { allowedHost },
        });

        var middleware = new PageSpeedMiddleware(
            _ => Task.CompletedTask, optionsMonitor,
            Substitute.For<IHtmlProcessor>(), cache, notifier,
            Substitute.For<ILogger<PageSpeedMiddleware>>(), hostFiltering);

        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Path = "/page";
        context.Request.Scheme = "http";
        context.Request.Host = new HostString("localhost");
        context.Request.Headers.Accept = accept;
        if (cookie != null) context.Request.Headers.Cookie = cookie;
        var responseBody = new MemoryStream();
        context.Response.Body = responseBody;

        await middleware.InvokeAsync(context);

        return (context.Response.StatusCode, context.Response.ContentType,
                responseBody.ToArray(), context.Response.Headers);
    }

    /// <summary>
    /// Fake cache: ReadBest returns HTML; ReadBestAgent returns the 0x7C
    /// markdown variant (only reached when the middleware computed entitled==1);
    /// ReadSharedConfigAgentEntitled returns the configured worker entitlement.
    /// </summary>
    private sealed class MarkdownNegotiationCache : IPageSpeedCache
    {
        private readonly int _agentEntitled;
        public MarkdownNegotiationCache(int agentEntitled) => _agentEntitled = agentEntitled;

        public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask) =>
            new NegotiationReadResult
            {
                ContentMemory = HtmlBody,
                Mask = 0x08,
                ContentType = PageSpeedContentType.Html,
                OriginContentType = "text/html; charset=utf-8",
            };

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled) =>
            new NegotiationReadResult
            {
                ContentMemory = MarkdownBody,
                Mask = SentinelId.AgentMarkdown, // (Mask & 0xFF) == 0x7C
                ContentType = PageSpeedContentType.Other,
                OriginContentType = null,
            };

        public int ReadSharedConfigAgentEntitled() => _agentEntitled;

        // The page recorded for the URL (the default alternate, 0x08), or
        // null for none.
        public IReadResult? RecordedPage { get; init; }

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId) =>
            alternateId == 0x08 ? RecordedPage : null;
        public IReadResult? ReadEarlyHints(string url, string hostname, string scheme) => null;
        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException();
        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId, ulong contentLength) =>
            throw new NotSupportedException();
        public CacheStats GetStats() => new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) { }
        SafeCacheHandle IPageSpeedCache.Handle => throw new NotSupportedException();
        public void Dispose() { }
    }

    private sealed class NegotiationReadResult : IReadResult
    {
        public ReadOnlyMemory<byte> ContentMemory { get; init; }
        public uint Mask { get; init; }
        public PageSpeedContentType ContentType { get; init; }
        public string? OriginContentType { get; init; }
        public byte Flags { get; init; } = CacheFlags.HeadersClassified | CacheFlags.AnonymousRecorded;
        public bool NeedsRevalidation => false;
        public ushort OriginCcFlags { get; init; }
        public uint OriginSMaxage { get; init; }
        public bool RevalidationRequired => false;
        public uint OriginContentLength { get; init; }
        public bool IsWorkerProcessed { get; init; }
        public uint CacheInsertedAt { get; init; }
        // No native borrow behind the fake — nothing to renew.
        public bool RenewLease() => false;
        public void Dispose() { }
    }
}

/// <summary>
/// Makes libpagespeed loadable for the native-gated negotiation cases. OPT-IN
/// only (matching <see cref="ImageVariantContentNegotiationTests"/>): set
/// PAGESPEED_AGENT_NATIVE_LIB to a built libpagespeed and these cases run by
/// staging it into the test's runtimes/&lt;rid&gt;/native layout (where the
/// library's own [ModuleInitializer] resolver looks first). Returns false
/// (=> SKIP) otherwise. We deliberately do NOT auto-discover a Bazel build:
/// loading the lib is process-global, and a sibling health test asserts the
/// native lib is absent — auto-loading would break it.
/// </summary>
internal static class NativeMarkdownProbe
{
    private static readonly object Gate = new();
    private static bool _evaluated;
    private static bool _available;

    public static bool Available(ITestOutputHelper output)
    {
        lock (Gate)
        {
            if (_evaluated) return _available;
            _evaluated = true;
            _available = TryEnable(output);
            if (!_available)
                output.WriteLine(
                    "SKIP: set PAGESPEED_AGENT_NATIVE_LIB to a built "
                    + "libpagespeed (e.g. bazel-bin/lib/pagespeed/libpagespeed.so) "
                    + "to run the native-gated markdown negotiation cases.");
            return _available;
        }
    }

    private static string? _stageNote;

    /// <summary>
    /// Stages the env-pointed library into the test output's
    /// runtimes/&lt;rid&gt;/native layout, where the product resolver (set once
    /// per assembly by its own module initializer) looks. This runs when the
    /// test assembly loads, before any test has mapped that file, so every
    /// test in the process sees the same library. The file is never written
    /// in place: it is copied to a unique name in the same directory and
    /// renamed over the target, so a mapping of the old file (if there were
    /// one) stays valid instead of being truncated underneath the process.
    /// </summary>
    [ModuleInitializer]
    internal static void StageEnvNativeLib()
    {
        var src = Environment.GetEnvironmentVariable("PAGESPEED_AGENT_NATIVE_LIB");
        if (string.IsNullOrWhiteSpace(src) || !File.Exists(src)) return;
        try
        {
            var rid = RuntimeInformation.RuntimeIdentifier;
            var nativeName =
                RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "pagespeed.dll"
                : RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "libpagespeed.dylib"
                : "libpagespeed.so";
            var dest = Path.Combine(
                AppContext.BaseDirectory, "runtimes", rid, "native", nativeName);
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            // The build may have staged a RELEASED NativeAssets lib that
            // predates ps_wants_agent_markdown; the env-pointed feature build
            // is authoritative here.
            var tmp = dest + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.Copy(src, tmp, overwrite: false);
            File.Move(tmp, dest, overwrite: true);
            _stageNote = $"staged native lib {src} -> {dest}";
        }
        catch (Exception ex)
        {
            _stageNote = $"staging native lib failed: {ex.GetType().Name}: {ex.Message}";
        }
    }

    private static bool TryEnable(ITestOutputHelper output)
    {
        var src = Environment.GetEnvironmentVariable("PAGESPEED_AGENT_NATIVE_LIB");
        if (string.IsNullOrWhiteSpace(src) || !File.Exists(src)) return false;
        try
        {
            if (_stageNote is not null) output.WriteLine(_stageNote);
            // Probe the actual Accept parser the negotiation depends on
            // (catches EntryPointNotFound if a stale lib somehow loaded first).
            return NativePageSpeed.WantsAgentMarkdown("text/markdown") == 1;
        }
        catch (Exception ex)
        {
            output.WriteLine($"native probe failed: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
    }
}

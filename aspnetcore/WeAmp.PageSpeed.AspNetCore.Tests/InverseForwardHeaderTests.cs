using FluentAssertions;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1 — outbound forwarded-request header correctness via the
/// InverseForwardTransformer applied to a synthesized request (nginx-free).
/// </summary>
public class InverseForwardHeaderTests
{
    private const string Dest = "http://127.0.0.1:5100";

    private static async Task<HttpRequestMessage> Transform(
        InternalSidecarEndpoint endpoint,
        string scheme = "https",
        string host = "shop.example.com",
        string path = "/index.html",
        string remoteIp = "203.0.113.9",
        IDictionary<string, string>? headers = null,
        HttpContent? content = null)
    {
        var ctx = InverseTestSupport.NewContext(
            scheme: scheme, host: host, path: path, remoteIp: remoteIp, headers: headers);

        var transformer = new InverseForwardTransformer(endpoint);
        var proxyReq = new HttpRequestMessage(HttpMethod.Get, Dest + path) { Content = content };

        await transformer.TransformRequestAsync(ctx, proxyReq, Dest, CancellationToken.None);
        return proxyReq;
    }

    private static string? First(HttpRequestMessage req, string name) =>
        req.Headers.TryGetValues(name, out var v) ? v.FirstOrDefault() : null;

    private static int Count(HttpRequestMessage req, string name) =>
        req.Headers.TryGetValues(name, out var v) ? v.Count() : 0;

    [Fact]
    public async Task ForwardedRequest_PreservesHostHeader()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint());
        req.Headers.Host.Should().Be("shop.example.com",
            "the upstream Host must be the client host (NOT 127.0.0.1) for cache-key/URL correctness");
        req.Headers.Host.Should().NotContain("127.0.0.1");
    }

    [Fact]
    public async Task ForwardedRequest_SetsXForwardedProto_FromHttpsScheme()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(), scheme: "https");
        First(req, InverseForwardTransformer.ForwardedProtoHeader).Should().Be("https");
    }

    [Fact]
    public async Task ForwardedRequest_SetsXForwardedProto_FromHttpScheme()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(), scheme: "http");
        First(req, InverseForwardTransformer.ForwardedProtoHeader).Should().Be("http");
    }

    [Fact]
    public async Task ForwardedRequest_OverwritesClientSuppliedXForwardedProto()
    {
        // CRITICAL: a client X-Forwarded-Proto: http on an https public request must
        // be REMOVED and replaced with a SINGLE 'https' (first-match/mixed-content guard).
        var req = await Transform(InverseTestSupport.NewEndpoint(), scheme: "https",
            headers: new Dictionary<string, string>
            {
                [InverseForwardTransformer.ForwardedProtoHeader] = "http"
            });

        Count(req, InverseForwardTransformer.ForwardedProtoHeader).Should().Be(1,
            "exactly one validated XFP must be present (no appended client value)");
        First(req, InverseForwardTransformer.ForwardedProtoHeader).Should().Be("https",
            "the client-supplied http must be overwritten with the real https scheme");
    }

    [Theory]
    [InlineData("ftp")]
    [InlineData("ws")]
    [InlineData("gopher")]
    [InlineData("")]
    public async Task ForwardedRequest_XForwardedProto_OnlyHttpOrHttps(string bogusScheme)
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(), scheme: bogusScheme);
        First(req, InverseForwardTransformer.ForwardedProtoHeader).Should().Be("https",
            "a non-http/https scheme normalizes to https (fail-safe, never downgrade)");
    }

    [Fact]
    public async Task ForwardedRequest_AppendsXForwardedFor()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(), remoteIp: "203.0.113.9");
        First(req, InverseForwardTransformer.ForwardedForHeader).Should().Contain("203.0.113.9");
    }

    [Fact]
    public async Task ForwardedRequest_SetsHopHeaderToOne_OnFirstForward()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint());
        First(req, InverseForwardTransformer.HopHeader).Should().Be("1");
    }

    [Fact]
    public async Task ForwardedRequest_IncrementsHopHeader_WhenAlreadyPresent()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(),
            headers: new Dictionary<string, string> { [InverseForwardTransformer.HopHeader] = "1" });
        First(req, InverseForwardTransformer.HopHeader).Should().Be("2");
        Count(req, InverseForwardTransformer.HopHeader).Should().Be(1, "the hop header must be replaced, not appended");
    }

    [Fact]
    public async Task ForwardedRequest_InjectsInternalNonce()
    {
        var ep = InverseTestSupport.NewEndpoint();
        var req = await Transform(ep);
        var expected = InverseForwardTransformer.EncodeNonce(ep.InternalNonce);
        First(req, InverseForwardTransformer.InternalHeader).Should().Be(expected);
    }

    [Fact]
    public async Task ForwardedRequest_StripsAcceptEncodingToIdentity()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(),
            headers: new Dictionary<string, string> { ["Accept-Encoding"] = "gzip, br" });
        req.Headers.Contains("Accept-Encoding").Should().BeFalse(
            "Accept-Encoding must be stripped so nginx receives parseable identity-encoded bytes");
    }

    [Fact]
    public async Task ForwardedRequest_DoesNotForwardHopByHopHeaders()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(),
            headers: new Dictionary<string, string>
            {
                ["Connection"] = "keep-alive",
                ["Keep-Alive"] = "timeout=5",
                ["TE"] = "trailers",
            });
        req.Headers.Contains("Connection").Should().BeFalse();
        req.Headers.Contains("Keep-Alive").Should().BeFalse();
        req.Headers.Contains("TE").Should().BeFalse();
    }

    [Fact]
    public async Task ForwardedRequest_DoesNotInjectModPagespeedUA()
    {
        var req = await Transform(InverseTestSupport.NewEndpoint(),
            headers: new Dictionary<string, string> { ["User-Agent"] = "Mozilla/5.0 RealBrowser" });
        var ua = First(req, "User-Agent") ?? string.Empty;
        ua.Should().NotContain("mod_pagespeed",
            "injecting the mod_pagespeed UA would make nginx skip optimizing the top-level HTML");
    }
}

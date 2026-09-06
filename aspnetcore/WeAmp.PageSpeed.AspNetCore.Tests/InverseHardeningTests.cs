// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using Microsoft.AspNetCore.Http;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Regression tests for the three must-fixes from the adversarial code review of
/// the Inverse-mode implementation:
///   (1) admin-404 must survive leading/internal '//' slash-folding (nginx
///       merge_slashes would otherwise route '//pagespeed_admin' to the admin
///       handler past the un-normalized middleware gate);
///   (2) a client-supplied X-PageSpeed-Hop on the PUBLIC port (non-loopback peer)
///       must be stripped so a client cannot force the loop-ceiling 500 / skip
///       optimization — while a loopback-leg hop is preserved so the backstop
///       still bounds a marker+port double-failure;
///   (3) Host handling: forward-all is the zero-config DEFAULT (the module
///       implicitly authorizes each request's own same-origin resources, and the
///       Host-keyed cache surface is bounded + Host-fragmented — parity with
///       running nginx+pagespeed in front of the app). The strict allowlist is an
///       OFF-by-default opt-in (Sidecar.RestrictToAuthorizedHosts); loopback is
///       ALWAYS allowed. (Revised from the original default-deny per the design record
///       forward-all analysis — see the ADR amendment.)
/// </summary>
public class InverseHardeningTests
{
    private static PageSpeedOptions OptionsWithDomains(params string[] domains)
    {
        var opts = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse },
            Domains = new DomainOptions(),
        };
        opts.Domains.AuthorizedDomains.Clear();
        foreach (var d in domains) opts.Domains.AuthorizedDomains.Add(d);
        return opts;
    }

    // Strict-allowlist (OFF-by-default opt-in): forward ONLY authorized hosts.
    private static PageSpeedOptions RestrictOptions(params string[] domains)
    {
        var opts = OptionsWithDomains(domains);
        opts.Sidecar.RestrictToAuthorizedHosts = true;
        return opts;
    }

    // ---- (1) admin-404 slash-folding ------------------------------------

    [Theory]
    [InlineData("//pagespeed_admin")]
    [InlineData("///pagespeed_statistics")]
    [InlineData("/pagespeed_admin//x")]
    public async Task AdminPath_SlashFolded_Is404_NotForwarded(string path)
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        // Default options (AllowPublicAdmin = false) → admin must 404 from public.
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(path: path);

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0, "a slash-folded admin path must not be forwarded to nginx");
        ctx.Response.StatusCode.Should().Be(StatusCodes.Status404NotFound);
    }

    [Fact]
    public void NormalizePath_CollapsesRepeatedSlashes()
    {
        PageSpeedInverseMiddleware.NormalizePath("//pagespeed_admin").Should().Be("/pagespeed_admin");
        PageSpeedInverseMiddleware.NormalizePath("/pagespeed_admin//x").Should().Be("/pagespeed_admin/x");
        PageSpeedInverseMiddleware.NormalizePath("/normal/path").Should().Be("/normal/path");
    }

    // ---- (2) public-port hop hygiene ------------------------------------

    [Fact]
    public async Task PublicPort_NonLoopbackClient_HighHop_IsForwarded_Not500()
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        // A real external client forging X-PageSpeed-Hop must NOT be able to force a 500.
        var ctx = InverseTestSupport.NewContext(
            path: "/", remoteIp: "203.0.113.7",
            headers: new Dictionary<string, string> { ["X-PageSpeed-Hop"] = "9" });

        await mw.InvokeAsync(ctx);

        ctx.Response.StatusCode.Should().NotBe(StatusCodes.Status500InternalServerError);
        fwd.SendCount.Should().Be(1, "the client hop is stripped for non-loopback peers; the request forwards normally");
    }

    [Fact]
    public async Task LoopbackPeer_HopAtCeiling_HardRejects500()
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        // A loopback-leg request whose hop already reached the ceiling is a loop
        // backstop trip → 500, never forwarded. (Hop preserved on loopback peers.)
        var ctx = InverseTestSupport.NewContext(
            path: "/", remoteIp: "127.0.0.1",
            headers: new Dictionary<string, string> { ["X-PageSpeed-Hop"] = "2" });

        await mw.InvokeAsync(ctx);

        ctx.Response.StatusCode.Should().Be(StatusCodes.Status500InternalServerError);
        fwd.SendCount.Should().Be(0);
    }

    // ---- (3) Host handling: forward-all default + opt-in strict allowlist ----

    [Fact]
    public async Task ForwardAll_IsTheDefault_AnyHostIsForwarded()
    {
        // Default options: RestrictToAuthorizedHosts is false → every host the app
        // serves is forwarded (the module implicitly authorizes same-origin).
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep); // default: forward-all
        var ctx = InverseTestSupport.NewContext(host: "random.test", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "forward-all is the zero-config default");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Fact]
    public async Task ForwardAll_HostNotInAuthorizedDomains_IsStillForwarded()
    {
        // A host NOT in AuthorizedDomains is forwarded by default — the old
        // default-deny gate is gone; AuthorizedDomains is inert unless restrict is on.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var opts = new PageSpeedOptions
        {
            Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse },
            Domains = new DomainOptions(), // default [localhost, 127.0.0.1]
        };
        var mw = InverseTestSupport.NewMiddleware(fwd, ep, opts);
        var ctx = InverseTestSupport.NewContext(host: "shop.example.com", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "forward-all does not gate on AuthorizedDomains");
    }

    [Fact]
    public async Task EmptyHost_IsBypassed_NotForwarded()
    {
        // A Host-less request (HTTP/1.0 / blank Host) gives the module nothing to key
        // on, so it is served un-optimized even under forward-all.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(host: "", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0, "a blank Host has nothing for the module to key on");
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task RestrictMode_UnauthorizedHost_IsBypassed()
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep, RestrictOptions("example.com"));
        var ctx = InverseTestSupport.NewContext(host: "evil.example.com", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0, "restrict mode drops hosts outside the allowlist");
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue("an unauthorized Host is served un-optimized via _next");
    }

    [Fact]
    public async Task RestrictMode_AuthorizedHost_IsForwarded()
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep, RestrictOptions("example.com"));
        var ctx = InverseTestSupport.NewContext(host: "example.com", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1);
    }

    [Fact]
    public async Task RestrictMode_WildcardAuthorizedHost_IsForwarded()
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep, RestrictOptions("*.example.com"));
        var ctx = InverseTestSupport.NewContext(host: "shop.example.com", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1);
    }

    [Theory]
    [InlineData("localhost")]
    [InlineData("127.0.0.1")]
    public async Task RestrictMode_LoopbackAlwaysAllowed_EvenWhenNotListed(string host)
    {
        // AuthorizedDomains deliberately does NOT contain loopback; it must still
        // forward (local dev + the raw-origin loopback leg can never be locked out).
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep, RestrictOptions("example.com"));
        var ctx = InverseTestSupport.NewContext(host: host, path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "loopback is always authorized");
    }

    [Theory]
    [InlineData("example.com", "example.com", true)]
    [InlineData("example.com", "other.com", false)]
    [InlineData("*.example.com", "shop.example.com", true)]
    [InlineData("*.example.com", "example.com", false)]
    [InlineData("example.com", "localhost", true)]   // loopback always allowed
    [InlineData("example.com", "127.0.0.1", true)]   // loopback always allowed
    [InlineData("localhost", "localhost", true)]
    public void HostIsAuthorized_MatchesExactWildcardAndAlwaysLoopback(string authorized, string requestHost, bool expected)
    {
        var opts = OptionsWithDomains(authorized);
        PageSpeedInverseMiddleware.HostIsAuthorized(new HostString(requestHost), opts).Should().Be(expected);
    }
}

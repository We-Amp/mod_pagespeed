// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using Microsoft.AspNetCore.Http;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1 — the SECURITY CORE (runs every CI dim, nginx-free). Tests the loop-
/// break predicate, the hop ceiling, the nonce-is-never-trusted-alone guard, and
/// the loopback-RemoteIp hard reject on the raw-origin branch.
/// </summary>
public class InverseLoopGuardTests
{
    [Fact]
    public async Task RawOriginMarkerPresent_IsBypassed_NotReForwarded()
    {
        // A request carrying the IRawOriginMarker feature (set by the raw-origin
        // listener) must bypass to _next and NEVER be forwarded (no recursion).
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        // setMarker but LocalPort = public to prove the MARKER alone is sufficient.
        var ctx = InverseTestSupport.NewContext(setMarker: true, localPort: InverseTestSupport.PublicPort);

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0, "the raw-origin branch must never forward (no loop)");
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue("the raw origin serves via _next");
    }

    [Fact]
    public async Task RawOriginLocalPort_IsBypassed()
    {
        // No marker feature, but LocalPort == RawOriginLoopbackPort — the redundant
        // kernel-set second signal must also break the loop.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(localPort: InverseTestSupport.RawOriginPort);

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task PublicPort_NoMarker_IsForwarded()
    {
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(); // public port, no marker

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "a public optimizable request is the only class that forwards");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
        fwd.LastDestination.Should().Be($"http://127.0.0.1:{InverseTestSupport.NginxLoopbackPort}");
    }

    [Fact]
    public async Task Nonce_IsNeverTrustedAlone_OnPublicPort()
    {
        // CRITICAL negative: a public-port request bearing a spoofed (even VALID)
        // X-PageSpeed-Internal nonce but WITHOUT the marker/raw-origin-port must
        // STILL be forwarded (treated untrusted, nonce stripped) — never bypassed.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);

        var validNonce = InverseForwardTransformer.EncodeNonce(ep.InternalNonce);
        var ctx = InverseTestSupport.NewContext(headers: new Dictionary<string, string>
        {
            [InverseForwardTransformer.InternalHeader] = validNonce
        });

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "a spoofed nonce on the public port must NOT cause a bypass");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
        // The inbound nonce must have been stripped before forwarding (the
        // transformer re-injects the real one).
        ctx.Request.Headers.ContainsKey(InverseForwardTransformer.InternalHeader)
            .Should().BeFalse("the untrusted inbound nonce must be stripped on the public port");
    }

    [Fact]
    public async Task HopCeiling_RejectsAtDepthTwo()
    {
        // X-PageSpeed-Hop >= 2 must hard-reject 500 BEFORE any classify/forward —
        // the tamper-evident termination backstop, independent of identity.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(headers: new Dictionary<string, string>
        {
            [InverseForwardTransformer.HopHeader] = "2"
        });

        await mw.InvokeAsync(ctx);

        ctx.Response.StatusCode.Should().Be(StatusCodes.Status500InternalServerError);
        fwd.SendCount.Should().Be(0, "hop ceiling is read first, before any forward");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Fact]
    public async Task HopHeader_IsNotStrippedOnRawOriginBranch()
    {
        // On the raw-origin branch the hop header is read first as the backstop and
        // NOT stripped — it must still be present after _next runs.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        string? hopSeenByNext = null;
        var mw = InverseTestSupport.NewMiddleware(fwd, ep, next: ctx =>
        {
            hopSeenByNext = ctx.Request.Headers[InverseForwardTransformer.HopHeader];
            return Task.CompletedTask;
        });
        var ctx = InverseTestSupport.NewContext(rawOrigin: true, headers: new Dictionary<string, string>
        {
            [InverseForwardTransformer.HopHeader] = "1"
        });

        await mw.InvokeAsync(ctx);

        hopSeenByNext.Should().Be("1", "the hop header must NOT be stripped on the raw-origin branch");
    }

    [Fact]
    public async Task LoopbackRemoteIp_HardRejectsNonLoopbackOnRawOriginBranch()
    {
        // A raw-origin-marked context whose TRANSPORT peer is NOT loopback must be
        // hard-rejected (421 + abort), not silently bypassed.
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(rawOrigin: true, remoteIp: "203.0.113.7");

        await mw.InvokeAsync(ctx);

        ctx.Response.StatusCode.Should().Be(StatusCodes.Status421MisdirectedRequest);
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse("a non-loopback peer must not reach the origin");
        fwd.SendCount.Should().Be(0);
    }

    [Fact]
    public async Task PagespeedSubrequestUserAgent_IsRecognized_DefenseInDepthOnly()
    {
        // The mod_pagespeed UA is recognized as defense-in-depth on the PUBLIC port
        // (declines to forward) — but it is NEVER the sole forward-vs-bypass gate
        // (the marker/port already decided this is the public port).
        var fwd = new StubPageSpeedForwarder();
        var ep = InverseTestSupport.NewEndpoint();
        var mw = InverseTestSupport.NewMiddleware(fwd, ep);
        var ctx = InverseTestSupport.NewContext(headers: new Dictionary<string, string>
        {
            ["User-Agent"] = "Serf/1.1 mod_pagespeed/1.15.0"
        });

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0, "a mod_pagespeed UA on the public port declines to forward (DiD)");
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task ForwardedRequestReentry_TerminatesBounded()
    {
        // Differential: a fake forwarder that re-invokes the middleware (simulating
        // the nginx->raw-origin re-entry without the loop break ever firing) must
        // TERMINATE with a 500 at hop depth 2 — never StackOverflow. We drive the
        // re-entry by replaying the request through a fresh PUBLIC-port context with
        // the hop header the transformer would have set (inbound+1).
        var ep = InverseTestSupport.NewEndpoint();
        PageSpeedInverseMiddleware? mw = null;
        int invocations = 0;

        // The forwarder re-enters the middleware on a fresh PUBLIC-port context
        // carrying the incremented hop (as the transformer would inject), without
        // the loop break ever firing — proving the hop ceiling terminates the cycle.
        var reentrantForwarder = new ReentrantForwarder(() => mw!, () => invocations++);
        mw = InverseTestSupport.NewMiddleware(reentrantForwarder, ep);

        var ctx = InverseTestSupport.NewContext(); // public, no hop header (hop=0 → transformer sets 1)

        await mw.InvokeAsync(ctx);

        // Leg 1 forwards (transformer would set hop=1). Re-entry leg carries hop=1
        // and forwards again (transformer would set hop=2). Re-entry leg 2 carries
        // hop=2 → the inner middleware hard-rejects with 500. Bounded, no overflow.
        invocations.Should().BeLessThan(5, "re-entry must terminate quickly, not recurse unbounded");
        reentrantForwarder.TerminatedWith500.Should().BeTrue(
            "the re-entry must terminate with a 500 at hop depth >= 2 (the tamper-evident backstop)");
    }

    /// <summary>
    /// A forwarder that, instead of dialing nginx, re-enters the middleware on a
    /// fresh PUBLIC-port context carrying the hop header the transformer would
    /// inject (inbound+1). Proves the hop ceiling terminates a real cycle.
    /// </summary>
    private sealed class ReentrantForwarder : IPageSpeedForwarder
    {
        private readonly Func<PageSpeedInverseMiddleware> _mw;
        private readonly Action _onInvoke;
        private const int SafetyCeiling = 50; // independent guard against a true overflow
        public bool TerminatedWith500 { get; private set; }

        public ReentrantForwarder(Func<PageSpeedInverseMiddleware> mw, Action onInvoke)
        {
            _mw = mw;
            _onInvoke = onInvoke;
        }

        public async ValueTask<Yarp.ReverseProxy.Forwarder.ForwarderError> SendAsync(
            HttpContext context, string destinationPrefix, CancellationToken cancellationToken = default)
        {
            _onInvoke();

            // Simulate the transformer setting hop = inbound+1 for the next leg.
            var inboundHop = context.Request.Headers[InverseForwardTransformer.HopHeader].ToString();
            var depth = int.TryParse(inboundHop, out var n) ? n : 0;
            var nextHop = depth + 1;

            if (depth >= SafetyCeiling)
            {
                // The hop ceiling should have fired long before this; surface a
                // failure rather than recurse forever if the backstop regressed.
                throw new InvalidOperationException("re-entry exceeded safety ceiling — hop backstop did not fire");
            }

            // The re-entry arrives back on the PUBLIC port (no marker/raw-origin)
            // carrying the incremented hop — exactly the cycle the backstop guards.
            var reentry = InverseTestSupport.NewContext(headers: new Dictionary<string, string>
            {
                [InverseForwardTransformer.HopHeader] = nextHop.ToString(),
            });

            await _mw().InvokeAsync(reentry);

            if (reentry.Response.StatusCode == StatusCodes.Status500InternalServerError)
            {
                TerminatedWith500 = true;
            }
            return Yarp.ReverseProxy.Forwarder.ForwarderError.None;
        }
    }
}

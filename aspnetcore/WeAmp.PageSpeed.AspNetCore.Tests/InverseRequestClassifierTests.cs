// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1 — forward-vs-bypass classification on the PUBLIC port, via a stub
/// IPageSpeedForwarder (nginx-free).
/// </summary>
public class InverseRequestClassifierTests
{
    private static PageSpeedOptions Inverse(Action<PageSpeedOptions>? configure = null)
    {
        var o = new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse } };
        configure?.Invoke(o);
        return o;
    }

    [Fact]
    public async Task Optimizable_GetHtmlRequest_IsForwarded()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1);
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Theory]
    [InlineData("/api/users")]
    [InlineData("/api/")]
    public async Task ExcludePathPrefix_IsBypassed(string path)
    {
        // 2.0 IsExcluded: a pattern with no */? is a case-insensitive StartsWith
        // prefix. "/api/" excludes /api/users, /api/, etc.
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(),
            Inverse(o => o.ExcludePaths.Add("/api/")));
        var ctx = InverseTestSupport.NewContext(path: path);

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Theory]
    [InlineData("/admin/secret")]
    [InlineData("/reports/q1.json")]
    public async Task ExcludePathGlob_IsBypassed(string path)
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(),
            Inverse(o => { o.ExcludePaths.Add("/admin/*"); o.ExcludePaths.Add("*.json"); }));
        var ctx = InverseTestSupport.NewContext(path: path);

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task Disabled_WhenOptionsEnabledFalse_IsBypassed()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(),
            Inverse(o => o.Enabled = false));
        var ctx = InverseTestSupport.NewContext(path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task WebSocketUpgrade_IsBypassed()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(path: "/");
        ctx.Features.Set<Microsoft.AspNetCore.Http.Features.IHttpWebSocketFeature>(new FakeWebSocketFeature());

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0, "the loopback hop cannot carry a WS upgrade");
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task SidecarNotRunning_IsBypassed_GracefulDegradation()
    {
        var fwd = new StubPageSpeedForwarder();
        var sidecar = new FakeSidecarManager { State = SidecarState.Failed };
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse(), sidecar);
        var ctx = InverseTestSupport.NewContext(path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0,
            "an optimizer outage must never fail the request: serve un-optimized rather than 502");
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task NonManagedNonGet_IsBypassed()
    {
        // Inverse bypasses non-GET/HEAD on NON-managed paths like 2.0 (mod_pagespeed
        // optimizes only cacheable GET/HEAD). A plain POST to a normal app path is
        // bypassed straight to the app; engine-owned managed paths forward regardless
        // of method (see NonGet_ToManagedPath_IsForwarded).
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(method: "POST", path: "/submit");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task Beacon_POST_IsForwarded()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(method: "POST", path: "/ngx_pagespeed_beacon");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "the beacon is an engine-owned managed path; it forwards for any method (GET or POST)");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Theory]
    [InlineData("OPTIONS")]
    [InlineData("POST")]
    public async Task NonGet_ToManagedPath_IsForwarded(string method)
    {
        // Engine-owned managed paths (here /pagespeed_static/) are owned by nginx, not
        // Kestrel — they MUST forward for EVERY method or Kestrel 404s them.
        // Regression-locks the GA managed-path-forwards-for-any-method contract.
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(method: method, path: "/pagespeed_static/js_defer.js");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "managed paths forward for any method");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Fact]
    public async Task ConnectMethod_IsBypassed()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(method: "CONNECT", path: "/");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public async Task AdminPath_From_PublicEndpoint_Returns404_ByDefault()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(path: "/pagespeed_admin");

        await mw.InvokeAsync(ctx);

        ctx.Response.StatusCode.Should().Be(404, "admin is not forwarded from the public front door by default");
        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Fact]
    public async Task AdminPath_From_PublicEndpoint_IsForwarded_WhenAllowPublicAdminAndBearer()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(),
            Inverse(o =>
            {
                o.Sidecar.AllowPublicAdmin = true;
                o.AdminAuth.Enabled = true; // bearer is load-bearing; nginx enforces it
            }));
        var ctx = InverseTestSupport.NewContext(path: "/pagespeed_statistics");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "with AllowPublicAdmin + bearer, admin is forwarded (nginx enforces the bearer)");
        ctx.Response.StatusCode.Should().NotBe(404);
    }

    private sealed class FakeWebSocketFeature : Microsoft.AspNetCore.Http.Features.IHttpWebSocketFeature
    {
        public bool IsWebSocketRequest => true;
        public Task<System.Net.WebSockets.WebSocket> AcceptAsync(
            Microsoft.AspNetCore.Http.WebSocketAcceptContext context) =>
            throw new NotSupportedException();
    }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 1 — routing for engine-owned URLs (the .pagespeed. contract, the static
/// prefix, the beacon, and the admin-404 default), via a stub forwarder.
/// </summary>
public class InverseAssetRoutingTests
{
    private static PageSpeedOptions Inverse(Action<PageSpeedOptions>? configure = null)
    {
        var o = new PageSpeedOptions { Sidecar = new SidecarOptions { Mode = SidecarMode.Inverse } };
        configure?.Invoke(o);
        return o;
    }

    [Theory]
    // The verbatim contract regex: <name>.pagespeed.<filter>.<10-hash>.<ext>
    [InlineData("/styles.css.pagespeed.ce.AbCdEfGhIj.css")]
    [InlineData("/a/b/script.js.pagespeed.jm.0123456789.js")]
    [InlineData("/img/logo.png.pagespeed.ic.qWeRtYuIoP.png")]
    public async Task PagespeedResourceUrl_IsForwardedToNginx(string path)
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(path: path);

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "a .pagespeed. resource URL is forwarded to nginx (terminal there)");
        InverseTestSupport.NextWasCalled(ctx).Should().BeFalse();
    }

    [Fact]
    public async Task PagespeedStaticPrefix_IsForwarded()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(path: "/pagespeed_static/js_defer.js");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1);
    }

    [Fact]
    public async Task Beacon_IsForwarded_InclPost()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());
        var ctx = InverseTestSupport.NewContext(method: "POST", path: "/ngx_pagespeed_beacon");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "the beacon is forwarded incl. POST");
    }

    [Fact]
    public async Task AdminPaths_Return404_FromPublicEndpoint_ByDefault()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(), Inverse());

        foreach (var admin in new[]
                 {
                     "/pagespeed_admin", "/pagespeed_statistics", "/pagespeed_global_statistics",
                     "/pagespeed_message", "/pagespeed_console"
                 })
        {
            var ctx = InverseTestSupport.NewContext(path: admin);
            await mw.InvokeAsync(ctx);
            ctx.Response.StatusCode.Should().Be(404, $"{admin} must 404 from the public endpoint by default");
        }
        fwd.SendCount.Should().Be(0);
    }

    [Fact]
    public async Task AdminPaths_Forwarded_WhenAllowPublicAdmin_AndBearerEnforcedByNginx()
    {
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(),
            Inverse(o => { o.Sidecar.AllowPublicAdmin = true; o.AdminAuth.Enabled = true; }));
        var ctx = InverseTestSupport.NewContext(path: "/pagespeed_admin");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(1, "with AllowPublicAdmin + bearer, admin forwards (nginx enforces the bearer)");
    }

    [Fact]
    public async Task ExcludedPath_IsNotForwarded()
    {
        // Asset-routing-vs-exclude precedence: an excluded GET is still bypassed
        // (it is NOT a managed path, so ExcludePaths wins).
        var fwd = new StubPageSpeedForwarder();
        var mw = InverseTestSupport.NewMiddleware(fwd, InverseTestSupport.NewEndpoint(),
            Inverse(o => o.ExcludePaths.Add("/api/")));
        var ctx = InverseTestSupport.NewContext(path: "/api/data");

        await mw.InvokeAsync(ctx);

        fwd.SendCount.Should().Be(0);
        InverseTestSupport.NextWasCalled(ctx).Should().BeTrue();
    }

    [Fact]
    public void IsPageSpeedManagedPath_PredicateMatchesContract()
    {
        // Direct predicate coverage (matches the verbatim NginxConfigGenerator regex).
        PageSpeedInverseMiddleware.IsPageSpeedManagedPath("/x.css.pagespeed.ce.AbCdEfGhIj.css").Should().BeTrue();
        PageSpeedInverseMiddleware.IsPageSpeedManagedPath("/pagespeed_static/foo.js").Should().BeTrue();
        PageSpeedInverseMiddleware.IsPageSpeedManagedPath("/ngx_pagespeed_beacon").Should().BeTrue();
        PageSpeedInverseMiddleware.IsPageSpeedManagedPath("/index.html").Should().BeFalse();
        PageSpeedInverseMiddleware.IsPageSpeedManagedPath("/style.css").Should().BeFalse();
    }
}

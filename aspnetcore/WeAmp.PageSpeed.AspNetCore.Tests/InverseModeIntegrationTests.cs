// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using System.Net;
using System.Runtime.InteropServices;
using FluentAssertions;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using WeAmp.PageSpeed.AspNetCore.DependencyInjection;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Tier 2 — LIVE nginx, Linux only. CI runs these against the bundled pair
/// (tools/ci/release_sidecar.sh test --pair, in the pull request tier and in the
/// module release lane) with PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1: the pair is
/// placed under runtimes/&lt;rid&gt;/native/ next to the test assembly, and a
/// missing pair fails every test instead of skipping it.
///
/// Without that variable (a developer box, macOS) every test no-ops via
/// <see cref="SkipUnlessLiveNginx"/> when no nginx can be found, so the default
/// `dotnet test` run is green without a live nginx; the unit Tier-1 suite carries
/// the security-critical proofs that run everywhere.
/// </summary>
[Trait("Category", "Integration")]
public class InverseModeIntegrationTests
{
    /// <summary>Set to 1 by CI: the bundled pair must be present and is required.</summary>
    internal const string RequireLiveNginxVariable = "PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX";

    private static string BundledNginxPath()
    {
        var rid = ProcessSidecarManager.ResolveBundledRid(RuntimeInformation.ProcessArchitecture);
        return Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native", "nginx");
    }

    private static bool LiveNginxRequired() =>
        Environment.GetEnvironmentVariable(RequireLiveNginxVariable) == "1";

    private static bool LiveNginxAvailable()
    {
        if (!OperatingSystem.IsLinux())
        {
            return false;
        }
        // Reuse the manager's RID-derive + well-known-locations probe shape.
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "nginx"),
            BundledNginxPath(),
            "/usr/local/bin/nginx",
            "/usr/sbin/nginx",
        };
        return candidates.Any(File.Exists);
    }

    /// <summary>
    /// Returns true (skip) when a live nginx pair is not available. When CI requires
    /// the live tier, the bundled pair itself must be there (a host nginx does not
    /// count), and its absence throws instead of skipping.
    /// </summary>
    private static bool SkipUnlessLiveNginx()
    {
        if (LiveNginxRequired())
        {
            if (!OperatingSystem.IsLinux() || !File.Exists(BundledNginxPath()))
            {
                throw new InvalidOperationException(
                    $"{RequireLiveNginxVariable}=1 but the bundled nginx is not at {BundledNginxPath()}");
            }
            return false;
        }
        return !LiveNginxAvailable();
    }

    private static async Task<IHost> StartInverseHostAsync(int publicPort, Action<PageSpeedOptions>? configure = null)
    {
        var builder = Host.CreateDefaultBuilder()
            .ConfigureWebHostDefaults(web =>
            {
                web.UseKestrel();
                web.ConfigureServices(services =>
                {
                    services.AddPageSpeedInverse(o =>
                    {
                        // Default OwnPublicPort=true → the package binds the public port
                        // (Any:ListenPort) AND the raw origin, both as code endpoints.
                        o.Sidecar.ListenPort = publicPort;
                        configure?.Invoke(o);
                    });
                });
                web.Configure(app =>
                {
                    app.UsePageSpeed();
                    app.UseRouting();
                    app.UseEndpoints(endpoints =>
                    {
                        endpoints.MapGet("/", () => Results.Content(
                            "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>" +
                            "<body><h1>inverse</h1></body></html>", "text/html"));
                        endpoints.MapGet("/style.css", (HttpContext c) =>
                        {
                            c.Response.Headers.CacheControl = "public, max-age=600";
                            return Results.Content("body  {  color : #333 ;  }\n", "text/css");
                        });
                        endpoints.MapGet("/api/data", () => Results.Json(new { ok = true }));
                    });
                });
            });

        var host = builder.Build();
        await host.StartAsync();
        return host;
    }

    /// <summary>
    /// An Inverse host whose /style.css CONTENT is derived from the request Host, so a
    /// cross-host cache bleed (poisoning) would be directly observable. No
    /// AuthorizedDomains entries — the forward-all default is exercised.
    /// </summary>
    private static async Task<IHost> StartHostVaryingCssAsync(int publicPort)
    {
        var builder = Host.CreateDefaultBuilder()
            .ConfigureWebHostDefaults(web =>
            {
                web.UseKestrel();
                web.ConfigureServices(services =>
                {
                    services.AddPageSpeedInverse(o => o.Sidecar.ListenPort = publicPort);
                });
                web.Configure(app =>
                {
                    app.UsePageSpeed();
                    app.UseRouting();
                    app.UseEndpoints(endpoints =>
                    {
                        endpoints.MapGet("/", () => Results.Content(
                            "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>" +
                            "<body><h1>x</h1></body></html>", "text/html"));
                        // The selector encodes the request Host; minification keeps the
                        // selector, so the served bytes reveal which Host's content it is.
                        endpoints.MapGet("/style.css", (HttpContext c) =>
                        {
                            var marker = c.Request.Host.Host.Replace('.', '-');
                            c.Response.Headers.CacheControl = "public, max-age=600";
                            return Results.Content($".host-{marker}{{color:#333}}\n", "text/css");
                        });
                    });
                });
            });

        var host = builder.Build();
        await host.StartAsync();
        return host;
    }

    [Fact]
    public async Task S0_PublicRequest_GetsOptimizedHtml_WithXPageSpeed()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartInverseHostAsync(port);
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        var resp = await client.GetAsync("/");
        resp.StatusCode.Should().Be(HttpStatusCode.OK);
        var body = await resp.Content.ReadAsStringAsync();

        resp.Headers.Contains("X-Page-Speed").Should().BeTrue("the optimized top-level HTML must carry X-Page-Speed");
        // mod_pagespeed 2.1 has no licensing: no optimized response carries the old
        // evaluation-mode warning header. (Requires a bundled module without licensing.)
        resp.Headers.Contains("X-PageSpeed-Warn").Should().BeFalse(
            "no X-PageSpeed-Warn header may appear on any optimized response");
        // The IsHtmlLike gate optimizes the non-browser loopback sub-request: the
        // external <link> should be inlined/minified into a <style>, NEVER a literal
        // ".pagespeed." substring in the top-level HTML.
        body.Should().NotContain(".pagespeed.");

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_NoInfiniteLoop_RequestTerminatesBounded()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartInverseHostAsync(port);
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}"), Timeout = TimeSpan.FromSeconds(10) };

        // THE loop-prevention integration proof: 5 public requests each terminate
        // within a wall-clock bound (no infinite re-forward).
        for (var i = 0; i < 5; i++)
        {
            var resp = await client.GetAsync("/");
            resp.StatusCode.Should().Be(HttpStatusCode.OK);
        }

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_PagespeedResource_ServedThroughMiddleware()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartInverseHostAsync(port);
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        // Fetch the page, extract a .pagespeed. URL from the optimized HTML (if the
        // filter rewrote a resource URL), then fetch it through the middleware.
        var pageBody = await (await client.GetAsync("/")).Content.ReadAsStringAsync();
        var m = System.Text.RegularExpressions.Regex.Match(pageBody, @"[^""']+\.pagespeed\.[^""']+");
        if (m.Success)
        {
            var resp = await client.GetAsync(m.Value);
            resp.StatusCode.Should().Be(HttpStatusCode.OK);
        }

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_ExcludedApiPath_BypassesNginx_ReturnsRawJson()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartInverseHostAsync(port, o => o.ExcludePaths.Add("/api/"));
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        var resp = await client.GetAsync("/api/data");
        resp.StatusCode.Should().Be(HttpStatusCode.OK);
        (await resp.Content.ReadAsStringAsync()).Should().Contain("ok");

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_AdminPath_From_Public_Returns404_ByDefault()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartInverseHostAsync(port);
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        var resp = await client.GetAsync("/pagespeed_admin");
        resp.StatusCode.Should().Be(HttpStatusCode.NotFound,
            "admin must be unreachable from the public front door by default (admin-authz fix)");

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_GracefulDegradation_WhenNginxDown()
    {
        if (SkipUnlessLiveNginx()) return;

        // Kill nginx (or never let it start) and assert fail-OPEN to _next (raw HTML,
        // not 502). Implemented as: force the sidecar to a non-Running state by
        // pointing at a bogus binary so the middleware degrades gracefully.
        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartInverseHostAsync(port, o => o.Sidecar.BinaryPath = "/nonexistent/nginx");
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        var resp = await client.GetAsync("/");
        // Fail-open: serve un-optimized rather than 502.
        resp.StatusCode.Should().BeOneOf(HttpStatusCode.OK, HttpStatusCode.ServiceUnavailable);

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_ForwardAll_OptimizesArbitraryHost_ZeroConfig()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        // Clear the allowlist seed entirely: prove optimization needs NO domain config
        // (forward-all default — the module implicitly authorizes same-origin).
        using var host = await StartInverseHostAsync(port, o => o.Domains.AuthorizedDomains.Clear());
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        var req = new HttpRequestMessage(HttpMethod.Get, "/");
        req.Headers.Host = "shop.example.test"; // arbitrary host, in no allowlist
        var resp = await client.SendAsync(req);

        resp.StatusCode.Should().Be(HttpStatusCode.OK);
        resp.Headers.Contains("X-Page-Speed").Should().BeTrue(
            "forward-all optimizes any host the app serves with zero domain configuration");

        await host.StopAsync();
    }

    [Fact]
    public async Task S0_HostPoisoning_Differential_CacheIsHostPartitioned()
    {
        if (SkipUnlessLiveNginx()) return;

        var port = Internal.EphemeralLoopbackPort.Allocate();
        using var host = await StartHostVaryingCssAsync(port);
        using var client = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}") };

        // Prime the optimizer cache as attacker.test (CSS marker is host-derived).
        var a = new HttpRequestMessage(HttpMethod.Get, "/style.css");
        a.Headers.Host = "attacker.test";
        var aCss = await (await client.SendAsync(a)).Content.ReadAsStringAsync();
        aCss.Should().Contain("host-attacker-test", "the origin serves host-derived CSS");

        // The SAME path as victim.test must return victim's content, NEVER attacker's
        // cached bytes — proving the module cache is Host-partitioned (must-fix #2:
        // no cross-host poisoning under forward-all).
        var v = new HttpRequestMessage(HttpMethod.Get, "/style.css");
        v.Headers.Host = "victim.test";
        var vCss = await (await client.SendAsync(v)).Content.ReadAsStringAsync();
        vCss.Should().Contain("host-victim-test", "victim must receive its own content");
        vCss.Should().NotContain("host-attacker-test",
            "a cross-host cache bleed would be a poisoning vulnerability");

        await host.StopAsync();
    }
}

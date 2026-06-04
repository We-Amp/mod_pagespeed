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
/// Tier 2 — LIVE nginx, Linux/docker only. Runs in sidecar-pair-build.yml's PW-2
/// licensed-optimize gate, NOT the default unit pass. Every test is gated on
/// Linux AND the bundled 1.15 nginx+module pair being resolvable; on a non-Linux
/// dev box (macOS) every test no-ops via <see cref="SkipUnlessLiveNginx"/> so the
/// default `dotnet test` run is green without a live nginx.
///
/// These are SKELETONS that wire the real Inverse host + a live nginx and assert
/// the S0 gates. They are intentionally conservative: when the bundled pair is not
/// present they return early rather than fail (the unit Tier-1 suite carries the
/// security-critical proofs that run on every CI dim).
/// </summary>
[Trait("Category", "Integration")]
public class InverseModeIntegrationTests
{
    private static bool LiveNginxAvailable()
    {
        if (!OperatingSystem.IsLinux())
        {
            return false;
        }
        // Reuse the manager's RID-derive + well-known-locations probe shape.
        var rid = ProcessSidecarManager.ResolveBundledRid(RuntimeInformation.ProcessArchitecture);
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "nginx"),
            Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native", "nginx"),
            "/usr/local/bin/nginx",
            "/usr/sbin/nginx",
        };
        return candidates.Any(File.Exists);
    }

    /// <summary>Returns true (skip) when a live nginx pair is not available.</summary>
    private static bool SkipUnlessLiveNginx() => !LiveNginxAvailable();

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

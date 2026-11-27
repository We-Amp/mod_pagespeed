// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Collections.Concurrent;
using System.Net;
using System.Net.Http;
using System.Security.Claims;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.Routing.Matching;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The middleware stores a response only when the request was anonymous as
/// far as the application can tell: no <c>Authorization</c> or
/// <c>Proxy-Authorization</c> header, no authenticated user, and no
/// authorization requirement on the endpoint (unless it allows anonymous
/// access). A response to any other request is served to that client only,
/// and an entry not recorded under this rule is never served.
/// Runs the standalone middleware (no worker) with the real native cache, in
/// the documented order: <c>UsePageSpeed()</c> first, then routing and
/// authentication. Requires the native library; skips when it is absent.
/// </summary>
public class AnonymousRecordTests : IDisposable
{
    private const string UserHeader = "X-Test-User";
    private const string PassHeader = "X-Test-Pass";
    private const string SchemeName = "Test";

    private const string OpenCss = "header{background:#0a0a0a}";
    private const string HeldCss = "header{background:#123456}";
    private const string HeldMarker = "123456";

    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), $"ps_anon_{Guid.NewGuid():N}");

    private readonly ConcurrentDictionary<string, int> _served = new();

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
    }

    /// <summary>Signs in the user named by the <c>X-Test-User</c> header.</summary>
    private sealed class HeaderAuthHandler : AuthenticationHandler<AuthenticationSchemeOptions>
    {
        public HeaderAuthHandler(
            IOptionsMonitor<AuthenticationSchemeOptions> options,
            ILoggerFactory logger, UrlEncoder encoder)
            : base(options, logger, encoder) { }

        protected override Task<AuthenticateResult> HandleAuthenticateAsync()
        {
            var name = Request.Headers[UserHeader].ToString();
            if (string.IsNullOrEmpty(name))
                return Task.FromResult(AuthenticateResult.NoResult());
            var identity = new ClaimsIdentity(
                new[] { new Claim(ClaimTypes.Name, name) }, SchemeName);
            return Task.FromResult(AuthenticateResult.Success(
                new AuthenticationTicket(new ClaimsPrincipal(identity), SchemeName)));
        }
    }

    // A policy that admits a request by a fact other than a signed-in user.
    private static void PassPolicy(AuthorizationPolicyBuilder p) =>
        p.RequireAssertion(c =>
            c.Resource is HttpContext h && h.Request.Headers.ContainsKey(PassHeader));

    // A missing native library fails the test: the theories are skipped up
    // front when it is absent locally (NativeLibTheory), and required on CI.
    private async Task<WebApplication> StartAsync(
        Action<WebApplication> map, bool asyncCss = false,
        Action<WebApplication>? afterPageSpeed = null,
        Action<PageSpeedOptions>? options = null,
        bool explicitRouting = true,
        Action<IServiceCollection>? services = null)
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.WebHost.UseTestServer();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddPageSpeed(opts =>
        {
            opts.Cache.VolumePath = Path.Combine(_dir, "volume.dat");
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = false;  // standalone: no worker
            opts.Html.EnableAsyncCss = asyncCss;
            options?.Invoke(opts);
        });
        builder.Services.AddAuthentication(SchemeName)
            .AddScheme<AuthenticationSchemeOptions, HeaderAuthHandler>(SchemeName, null);
        builder.Services.AddAuthorization();
        services?.Invoke(builder.Services);
        var app = builder.Build();

        app.UsePageSpeed();
        afterPageSpeed?.Invoke(app);
        // Without an explicit call, the host inserts routing at the start of
        // the pipeline, ahead of UsePageSpeed().
        if (explicitRouting) app.UseRouting();
        app.UseAuthentication();
        app.UseAuthorization();
        map(app);

        await app.StartAsync();
        return app;
    }

    private RouteHandlerBuilder Css(WebApplication app, string path, string body) =>
        Css(app, path, _ => body);

    private RouteHandlerBuilder Css(
        WebApplication app, string path, Func<HttpContext, string> body) =>
        app.MapGet(path, (HttpContext ctx) =>
        {
            Count(ctx, path);
            ctx.Response.Headers.CacheControl = "public, max-age=3600";
            return Results.Text(body(ctx), "text/css");
        });

    private RouteHandlerBuilder Page(WebApplication app, string path, string html) =>
        app.MapGet(path, (HttpContext ctx) =>
        {
            Count(ctx, path);
            ctx.Response.Headers.CacheControl = "public, max-age=60";
            return Results.Text(html, "text/html");
        });

    private int Served(string path) => _served.TryGetValue(path, out var n) ? n : 0;

    // Requests from 224.0.0.0 are the middleware's own anonymous check before
    // a response is stored (AnonymousFetchCheck); Served counts the others.
    private readonly ConcurrentDictionary<string, int> _checks = new();

    private void Count(HttpContext ctx, string path)
    {
        var counter = AgentSelfFetch.Label.Equals(ctx.Connection.RemoteIpAddress)
            ? _checks : _served;
        counter.AddOrUpdate(path, 1, (_, n) => n + 1);
    }

    private int Checks(string path) => _checks.TryGetValue(path, out var n) ? n : 0;

    private static HttpClient Client(WebApplication app)
    {
        var client = app.GetTestClient();
        client.BaseAddress = new Uri("https://localhost/");
        return client;
    }

    private static HttpRequestMessage Get(string path, bool html,
        params (string Name, string Value)[] headers)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, path);
        if (html) request.Headers.Accept.ParseAdd("text/html");
        foreach (var (name, value) in headers)
            request.Headers.TryAddWithoutValidation(name, value);
        return request;
    }

    private static string XPageSpeed(HttpResponseMessage response) =>
        response.Headers.TryGetValues("X-PageSpeed", out var v)
            ? string.Join(",", v) : "";

    private static string CriticalBlock(string page)
    {
        var block = Regex.Match(page,
            "<style[^>]*data-pagespeed-critical[^>]*>(.*?)</style>",
            RegexOptions.Singleline);
        return block.Success ? block.Groups[1].Value : "";
    }

    private const string PageHtml =
        "<!doctype html><html><head>" +
        "<link rel=\"stylesheet\" href=\"/open.css\">" +
        "</head><body><header>Top</header><main><p>Hello</p></main>" +
        "</body></html>";

    // ── The request kinds whose responses are not stored ─────────────

    public static IEnumerable<object[]> Admitted()
    {
        foreach (var html in new[] { false, true })
        {
            yield return new object[] { "authorization-header", html };
            yield return new object[] { "signed-in-user", html };
            yield return new object[] { "authorize-endpoint", html };
        }
    }

    private static (string, string)[] AdmittedHeaders(string kind) => kind switch
    {
        // No handler reads Basic: the user stays anonymous, only the header
        // tells.
        "authorization-header" => new[] { ("Authorization", "Basic dXNlcjpwYXNz") },
        "signed-in-user" => new[] { (UserHeader, "alice") },
        // Admitted by the endpoint's policy without a signed-in user.
        "authorize-endpoint" => new[] { (PassHeader, "1") },
        _ => throw new ArgumentException(kind),
    };

    [NativeLibTheory]
    [MemberData(nameof(Admitted))]
    public async Task ResponseToNonAnonymousRequest_IsNotCached(string kind, bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            var target = html
                ? Page(a, path, PageHtml)
                : Css(a, path, HeldCss);
            if (kind == "authorize-endpoint") target.RequireAuthorization(PassPolicy);
        });
        try
        {
            var client = Client(app);
            // An anonymous visit records the page's stylesheet.
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();

            var first = await client.SendAsync(Get(path, html, AdmittedHeaders(kind)));
            Assert.Equal(HttpStatusCode.OK, first.StatusCode);
            var firstBody = await first.Content.ReadAsStringAsync();
            if (html)
            {
                // Still optimized for this client.
                Assert.Contains("0a0a0a", CriticalBlock(firstBody));
            }
            else
            {
                Assert.Contains(HeldMarker, firstBody);
            }
            Assert.Equal(1, Served(path));

            // A following anonymous request is a miss: the application answers.
            var second = await client.SendAsync(Get(path, html));
            var secondBody = await second.Content.ReadAsStringAsync();
            Assert.NotEqual("HIT", XPageSpeed(second));
            if (kind == "authorize-endpoint")
            {
                Assert.Equal(HttpStatusCode.Unauthorized, second.StatusCode);
                Assert.DoesNotContain(HeldMarker, secondBody);
                Assert.DoesNotContain("<header>", secondBody);
            }
            else
            {
                Assert.Equal(HttpStatusCode.OK, second.StatusCode);
                Assert.Equal(2, Served(path));
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // Positive control for the theory above: the same responses to anonymous
    // requests are stored and served from the cache, also on an endpoint
    // with an authorization requirement that allows anonymous access.
    [NativeLibTheory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task ResponseToAnonymousRequest_IsCached(bool html, bool allowAnonymous)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            var target = html ? Page(a, path, PageHtml) : Css(a, path, HeldCss);
            if (allowAnonymous) target.RequireAuthorization(PassPolicy).AllowAnonymous();
        });
        try
        {
            var client = Client(app);
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();
            (await client.SendAsync(Get(path, html))).EnsureSuccessStatusCode();
            var second = await client.SendAsync(Get(path, html));
            second.EnsureSuccessStatusCode();
            Assert.Equal("HIT", XPageSpeed(second));
            Assert.Equal(1, Served(path));
            // Asked once more as an anonymous client before it was stored.
            Assert.Equal(1, Checks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── A stylesheet held back from anonymous clients ───────────────

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CriticalCss_NeverFromAStylesheetFirstFetchedBySignedInClient(bool asyncCss)
    {
        const string page =
            "<!doctype html><html><head>" +
            "<link rel=\"stylesheet\" href=\"/open.css\">" +
            "<link rel=\"stylesheet\" href=\"/held.css\">" +
            "</head><body><header>Top</header><main><p>Hello</p></main>" +
            "</body></html>";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            Css(a, "/held.css", HeldCss).RequireAuthorization();
            Page(a, "/", page);
        }, asyncCss);
        try
        {
            var client = Client(app);
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();
            for (var i = 0; i < 3; i++)
            {
                var primed = await client.SendAsync(
                    Get("/held.css", false, (UserHeader, "alice")));
                Assert.Equal(HttpStatusCode.OK, primed.StatusCode);
                Assert.Contains(HeldMarker, await primed.Content.ReadAsStringAsync());
            }

            // The page for an anonymous client, built and then from the cache.
            for (var i = 0; i < 2; i++)
            {
                var response = await client.SendAsync(Get("/", true));
                response.EnsureSuccessStatusCode();
                var body = await response.Content.ReadAsStringAsync();
                Assert.Contains("0a0a0a", CriticalBlock(body));
                Assert.DoesNotContain(HeldMarker, body);
            }

            // The signed-in client still gets the stylesheet from the application.
            var own = await client.SendAsync(Get("/held.css", false, (UserHeader, "alice")));
            Assert.Equal(HttpStatusCode.OK, own.StatusCode);
            Assert.Contains(HeldMarker, await own.Content.ReadAsStringAsync());
            Assert.Equal(4, Served("/held.css"));

            // An anonymous client is refused by the application, never served
            // the stylesheet from the cache.
            var anonymous = await client.GetAsync("/held.css");
            Assert.Equal(HttpStatusCode.Unauthorized, anonymous.StatusCode);
            Assert.NotEqual("HIT", XPageSpeed(anonymous));
            Assert.DoesNotContain(HeldMarker, await anonymous.Content.ReadAsStringAsync());
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("alice")]
    public async Task CriticalCss_NeverCarriesAnotherUsersStylesheet(string user1)
    {
        // One URL, a body per signed-in user.
        const string page =
            "<!doctype html><html><head>" +
            "<link rel=\"stylesheet\" href=\"/theme.css\">" +
            "</head><body><header>Top</header><main><p>Hello</p></main>" +
            "</body></html>";
        var app = await StartAsync(a =>
        {
            Css(a, "/theme.css", ctx =>
                ctx.Request.Headers[UserHeader].ToString() == user1
                    ? "header{background:#aa0001}"
                    : "header{background:#bb0002}");
            Page(a, "/", page);
        });
        try
        {
            var client = Client(app);
            for (var i = 0; i < 3; i++)
            {
                (await client.SendAsync(Get("/theme.css", false, (UserHeader, user1))))
                    .EnsureSuccessStatusCode();
            }
            foreach (var user in new[] { "bob", "" })
            {
                var request = user.Length > 0
                    ? Get("/", true, (UserHeader, user))
                    : Get("/", true);
                var response = await client.SendAsync(request);
                response.EnsureSuccessStatusCode();
                Assert.DoesNotContain("aa0001", await response.Content.ReadAsStringAsync());
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Responses the application gives only to some clients ────────

    private static readonly IPAddress OfficePeer = IPAddress.Parse("192.0.2.10");
    private static readonly IPAddress OtherPeer = IPAddress.Parse("198.51.100.7");
    private const string KeyHeader = "X-Test-Key";

    // Anonymous requests the address rule below refused, by path.
    private readonly ConcurrentDictionary<string, int> _ruleChecks = new();

    private int RuleChecks(string path) =>
        _ruleChecks.TryGetValue(path, out var n) ? n : 0;

    // Middleware after UsePageSpeed() that admits /office/* only from
    // OfficePeer.
    private void OfficeAddressRule(WebApplication app) =>
        app.Use(async (ctx, next) =>
        {
            if (AgentSelfFetch.Label.Equals(ctx.Connection.RemoteIpAddress))
                _ruleChecks.AddOrUpdate(ctx.Request.Path.Value!, 1, (_, n) => n + 1);
            if (ctx.Request.Path.StartsWithSegments("/office") &&
                !OfficePeer.Equals(ctx.Connection.RemoteIpAddress))
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }
            await next();
        });

    private sealed record Answer(HttpStatusCode Status, string Body, string XPageSpeed);

    private static Task<Answer> SendFrom(
        WebApplication app, IPAddress peer, string path, bool html,
        params (string Name, string Value)[] headers) =>
        SendFrom(app, peer, path, html, null, 0, headers);

    private static async Task<Answer> SendFrom(
        WebApplication app, IPAddress peer, string path, bool html,
        int? hostPort, int localPort,
        params (string Name, string Value)[] headers)
    {
        var ctx = await app.GetTestServer().SendAsync(c =>
        {
            c.Request.Method = "GET";
            c.Request.Scheme = "https";
            c.Request.Host = hostPort is { } port
                ? new HostString("localhost", port)
                : new HostString("localhost");
            if (localPort != 0) c.Connection.LocalPort = localPort;
            c.Request.Path = path;
            if (html) c.Request.Headers.Accept = "text/html";
            foreach (var (name, value) in headers) c.Request.Headers[name] = value;
            c.Connection.RemoteIpAddress = peer;
        });
        using var reader = new StreamReader(ctx.Response.Body);
        return new Answer(
            (HttpStatusCode)ctx.Response.StatusCode,
            await reader.ReadToEndAsync(),
            ctx.Response.Headers["X-PageSpeed"].ToString());
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ResponseAdmittedByClientAddress_IsNotServedToOtherClients(bool html)
    {
        var path = html ? "/office/target.html" : "/office/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, afterPageSpeed: OfficeAddressRule);
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            for (var i = 0; i < 2; i++)
            {
                var admitted = await SendFrom(app, OfficePeer, path, html);
                Assert.Equal(HttpStatusCode.OK, admitted.Status);
                Assert.Contains(html ? "<header>" : HeldMarker, admitted.Body);
            }

            var other = await SendFrom(app, OtherPeer, path, html);
            Assert.Equal(HttpStatusCode.Forbidden, other.Status);
            Assert.NotEqual("HIT", other.XPageSpeed);
            Assert.DoesNotContain(HeldMarker, other.Body);
            Assert.DoesNotContain("<header>", other.Body);
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CriticalCss_NeverFromAStylesheetAdmittedByClientAddress(bool asyncCss)
    {
        const string page =
            "<!doctype html><html><head>" +
            "<link rel=\"stylesheet\" href=\"/open.css\">" +
            "<link rel=\"stylesheet\" href=\"/office/held.css\">" +
            "</head><body><header>Top</header><main><p>Hello</p></main>" +
            "</body></html>";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            Css(a, "/office/held.css", HeldCss);
            Page(a, "/", page);
        }, asyncCss, afterPageSpeed: OfficeAddressRule);
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            for (var i = 0; i < 3; i++)
            {
                var primed = await SendFrom(app, OfficePeer, "/office/held.css", false);
                Assert.Equal(HttpStatusCode.OK, primed.Status);
                Assert.Contains(HeldMarker, primed.Body);
            }

            // The page for another client, built and then from the cache.
            for (var i = 0; i < 2; i++)
            {
                var response = await SendFrom(app, OtherPeer, "/", true);
                Assert.Equal(HttpStatusCode.OK, response.Status);
                Assert.Contains("0a0a0a", CriticalBlock(response.Body));
                Assert.DoesNotContain(HeldMarker, response.Body);
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ResponseAdmittedByAppHeaderCheck_IsNotServedToOtherClients(bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            // The handler admits by a header the application reads itself.
            a.MapGet(path, (HttpContext ctx) =>
            {
                Count(ctx, path);
                if (!ctx.Request.Headers.ContainsKey(KeyHeader))
                    return Results.StatusCode(StatusCodes.Status403Forbidden);
                ctx.Response.Headers.CacheControl = "public, max-age=3600";
                return html
                    ? Results.Text(PageHtml.Replace("Hello", HeldMarker), "text/html")
                    : Results.Text(HeldCss, "text/css");
            });
            Css(a, "/open.css", OpenCss);
        });
        try
        {
            var client = Client(app);
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();
            for (var i = 0; i < 2; i++)
            {
                var keyed = await client.SendAsync(Get(path, html, (KeyHeader, "k")));
                Assert.Equal(HttpStatusCode.OK, keyed.StatusCode);
                Assert.Contains(HeldMarker, await keyed.Content.ReadAsStringAsync());
            }

            var other = await client.SendAsync(Get(path, html));
            Assert.Equal(HttpStatusCode.Forbidden, other.StatusCode);
            Assert.NotEqual("HIT", XPageSpeed(other));
            Assert.DoesNotContain(HeldMarker, await other.Content.ReadAsStringAsync());
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // Positive control: a response the address rule does not restrict is
    // stored when the first client to ask came from the admitted address.
    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OpenResponseFirstFetchedFromAdmittedAddress_IsCached(bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, afterPageSpeed: OfficeAddressRule);
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OfficePeer, path, html)).Status);
            var other = await SendFrom(app, OtherPeer, path, html);
            Assert.Equal(HttpStatusCode.OK, other.Status);
            Assert.Equal("HIT", other.XPageSpeed);
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── What is stored: the anonymous request's response ────────────

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task StoredResponse_IsTheAnonymousRequestsResponse(bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            // 200 for every client; a client with the key header gets more.
            a.MapGet(path, (HttpContext ctx) =>
            {
                Count(ctx, path);
                ctx.Response.Headers.CacheControl = "public, max-age=3600";
                var keyed = ctx.Request.Headers.ContainsKey(KeyHeader);
                return html
                    ? Results.Text(keyed ? PageHtml.Replace("Hello", HeldMarker) : PageHtml,
                                   "text/html")
                    : Results.Text(keyed ? HeldCss : OpenCss, "text/css");
            });
        });
        try
        {
            var client = Client(app);
            (await client.GetAsync("/open.css")).EnsureSuccessStatusCode();
            var keyed = await client.SendAsync(Get(path, html, (KeyHeader, "k")));
            Assert.Equal(HttpStatusCode.OK, keyed.StatusCode);
            // The visitor gets its own response.
            Assert.Contains(HeldMarker, await keyed.Content.ReadAsStringAsync());

            var other = await client.SendAsync(Get(path, html));
            var body = await other.Content.ReadAsStringAsync();
            Assert.Equal("HIT", XPageSpeed(other));
            Assert.DoesNotContain(HeldMarker, body);
            Assert.Contains(html ? "<p>Hello</p>" : "0a0a0a", body);
            Assert.Equal(1, Served(path));
            Assert.Equal(1, Checks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData("set-cookie")]
    [InlineData("private")]
    [InlineData("no-store")]
    public async Task AnonymousResponseThatMayNotBeStored_IsNotStored(string kind)
    {
        const string path = "/target.css";
        var app = await StartAsync(a =>
            a.MapGet(path, (HttpContext ctx) =>
            {
                Count(ctx, path);
                // A visitor with the key header gets a shareable response;
                // a request without it does not.
                var keyed = ctx.Request.Headers.ContainsKey(KeyHeader);
                ctx.Response.Headers.CacheControl = keyed || kind == "set-cookie"
                    ? "public, max-age=3600"
                    : kind + ", max-age=3600";
                if (!keyed && kind == "set-cookie")
                    ctx.Response.Headers.SetCookie = "s=new; Path=/";
                return Results.Text(OpenCss, "text/css");
            }));
        try
        {
            var client = Client(app);
            for (var i = 0; i < 2; i++)
            {
                var keyed = await client.SendAsync(Get(path, false, (KeyHeader, "k")));
                Assert.Equal(HttpStatusCode.OK, keyed.StatusCode);
                Assert.NotEqual("HIT", XPageSpeed(keyed));
            }
            Assert.Equal(2, Served(path));
            // Refused once, then remembered.
            Assert.Equal(1, Checks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RefusedUrl_IsNotAskedAgainOnEveryRequest(bool html)
    {
        var path = html ? "/office/target.html" : "/office/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, afterPageSpeed: OfficeAddressRule);
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            for (var i = 0; i < 4; i++)
            {
                var admitted = await SendFrom(app, OfficePeer, path, html);
                Assert.Equal(HttpStatusCode.OK, admitted.Status);
            }
            Assert.Equal(4, Served(path));
            // Refused once, then remembered for a while.
            Assert.Equal(1, RuleChecks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RecordResponsesOff_StoresNothingAndMakesNoSecondRequest(bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, options: o => o.RecordResponses = false);
        try
        {
            var client = Client(app);
            for (var i = 0; i < 2; i++)
            {
                var response = await client.SendAsync(Get(path, html));
                response.EnsureSuccessStatusCode();
                Assert.NotEqual("HIT", XPageSpeed(response));
            }
            Assert.Equal(2, Served(path));
            Assert.Equal(0, Checks(path));
            Assert.Equal(0, Checks("/open.css"));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // An endpoint the pipeline selects after UsePageSpeed() is selected again
    // for the anonymous request, from the URL that reached the middleware.
    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task EndpointSelectedAfterPageSpeed_IsSelectedAgainForTheAnonymousRequest(
        bool html)
    {
        var path = html ? "/theme.html" : "/theme.css";
        var officePath = "/office" + path;
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html)
            {
                Page(a, path, PageHtml);
                Page(a, officePath, PageHtml.Replace("Hello", HeldMarker));
            }
            else
            {
                Css(a, path, OpenCss);
                Css(a, officePath, HeldCss);
            }
        }, afterPageSpeed: a => a.Use(async (ctx, next) =>
        {
            // Clients from the office address get the office variant.
            if (OfficePeer.Equals(ctx.Connection.RemoteIpAddress) &&
                ctx.Request.Path == path)
                ctx.Request.Path = officePath;
            await next();
        }));
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            var admitted = await SendFrom(app, OfficePeer, path, html);
            Assert.Equal(HttpStatusCode.OK, admitted.Status);
            Assert.Contains(HeldMarker, admitted.Body);

            var other = await SendFrom(app, OtherPeer, path, html);
            Assert.Equal(HttpStatusCode.OK, other.Status);
            Assert.Equal("HIT", other.XPageSpeed);
            Assert.DoesNotContain(HeldMarker, other.Body);
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // The cache key has no port: a response one listener gives is not served
    // to clients of another.
    [NativeLibTheory]
    [InlineData("local-port", false)]
    [InlineData("local-port", true)]
    [InlineData("host-port", false)]
    [InlineData("host-port", true)]
    public async Task ResponseAdmittedByListenerPort_IsNotServedOnOtherPorts(
        string by, bool html)
    {
        var path = html ? "/office/target.html" : "/office/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, afterPageSpeed: a => a.Use(async (ctx, next) =>
        {
            var port = by == "local-port" ? ctx.Connection.LocalPort : ctx.Request.Host.Port;
            if (ctx.Request.Path.StartsWithSegments("/office") && port != 8443)
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return;
            }
            await next();
        }));
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            for (var i = 0; i < 2; i++)
            {
                var admitted = by == "local-port"
                    ? await SendFrom(app, OtherPeer, path, html, null, 8443)
                    : await SendFrom(app, OtherPeer, path, html, 8443, 0);
                Assert.Equal(HttpStatusCode.OK, admitted.Status);
                Assert.Contains(html ? "<header>" : HeldMarker, admitted.Body);
            }

            var other = by == "local-port"
                ? await SendFrom(app, OtherPeer, path, html, null, 443)
                : await SendFrom(app, OtherPeer, path, html);
            Assert.Equal(HttpStatusCode.Forbidden, other.Status);
            Assert.NotEqual("HIT", other.XPageSpeed);
            Assert.DoesNotContain(HeldMarker, other.Body);
            Assert.DoesNotContain("<header>", other.Body);
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Endpoints selected before UsePageSpeed() ────────────────────

    // Positive control: with routing ahead of UsePageSpeed() (no explicit
    // UseRouting()), a plain endpoint is still cached.
    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PlainEndpointSelectedBeforePageSpeed_IsCached(bool html)
    {
        var path = html ? "/target.html" : "/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, explicitRouting: false);
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            Assert.Equal(HttpStatusCode.OK, (await SendFrom(app, OtherPeer, path, html)).Status);
            var second = await SendFrom(app, OtherPeer, path, html);
            Assert.Equal("HIT", second.XPageSpeed);
            Assert.Equal(1, Checks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task EndpointMatchedByHostPortBeforePageSpeed_IsNotStored(bool html)
    {
        var path = html ? "/office/target.html" : "/office/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            var target = html ? Page(a, path, PageHtml) : Css(a, path, HeldCss);
            target.RequireHost("*:8443");
        }, explicitRouting: false);
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            for (var i = 0; i < 2; i++)
            {
                var admitted = await SendFrom(app, OtherPeer, path, html, 8443, 0);
                Assert.Equal(HttpStatusCode.OK, admitted.Status);
                Assert.Contains(html ? "<header>" : HeldMarker, admitted.Body);
            }

            var other = await SendFrom(app, OtherPeer, path, html);
            Assert.Equal(HttpStatusCode.NotFound, other.Status);
            Assert.NotEqual("HIT", other.XPageSpeed);
            Assert.DoesNotContain(HeldMarker, other.Body);
            Assert.DoesNotContain("<header>", other.Body);
            // Refused without a second request.
            Assert.Equal(0, Checks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // A matcher policy the middleware does not know, admitting /office/*
    // endpoints only for OfficePeer.
    private sealed class OfficeAddressMatcherPolicy : MatcherPolicy, IEndpointSelectorPolicy
    {
        public override int Order => 0;

        public bool AppliesToEndpoints(IReadOnlyList<Endpoint> endpoints) =>
            endpoints.Any(e => e is RouteEndpoint r &&
                               (r.RoutePattern.RawText ?? "").StartsWith("/office"));

        public Task ApplyAsync(HttpContext httpContext, CandidateSet candidates)
        {
            for (var i = 0; i < candidates.Count; i++)
            {
                if (!OfficePeer.Equals(httpContext.Connection.RemoteIpAddress))
                    candidates.SetValidity(i, false);
            }
            return Task.CompletedTask;
        }
    }

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task EndpointSelectedByUnknownMatcherPolicyBeforePageSpeed_IsNotStored(bool html)
    {
        var path = html ? "/office/target.html" : "/office/target.css";
        var app = await StartAsync(a =>
        {
            Css(a, "/open.css", OpenCss);
            if (html) Page(a, path, PageHtml); else Css(a, path, HeldCss);
        }, explicitRouting: false,
           services: s => s.AddSingleton<MatcherPolicy, OfficeAddressMatcherPolicy>());
        try
        {
            Assert.Equal(HttpStatusCode.OK,
                (await SendFrom(app, OtherPeer, "/open.css", false)).Status);
            for (var i = 0; i < 2; i++)
            {
                var admitted = await SendFrom(app, OfficePeer, path, html);
                Assert.Equal(HttpStatusCode.OK, admitted.Status);
                Assert.Contains(html ? "<header>" : HeldMarker, admitted.Body);
            }

            var other = await SendFrom(app, OtherPeer, path, html);
            Assert.Equal(HttpStatusCode.NotFound, other.Status);
            Assert.NotEqual("HIT", other.XPageSpeed);
            Assert.DoesNotContain(HeldMarker, other.Body);
            Assert.DoesNotContain("<header>", other.Body);
            Assert.Equal(0, Checks(path));
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // A HEAD miss says nothing about the GET that would be stored: its
    // refusal is not remembered.
    [NativeLibTheory]
    [InlineData("/target.css")]
    public async Task RefusedHeadMiss_DoesNotHoldBackTheGet(string path)
    {
        var app = await StartAsync(a =>
            a.MapMethods(path, new[] { "GET", "HEAD" }, async (HttpContext ctx) =>
            {
                Count(ctx, path);
                ctx.Response.Headers.CacheControl = "public, max-age=3600";
                ctx.Response.ContentType = "text/css";
                // This app writes no body for a HEAD to the anonymous request.
                if (HttpMethods.IsHead(ctx.Request.Method) &&
                    AgentSelfFetch.Label.Equals(ctx.Connection.RemoteIpAddress))
                    return;
                await ctx.Response.WriteAsync(OpenCss);
            }));
        try
        {
            var client = Client(app);
            var head = await client.SendAsync(new HttpRequestMessage(HttpMethod.Head, path));
            Assert.Equal(HttpStatusCode.OK, head.StatusCode);
            (await client.GetAsync(path)).EnsureSuccessStatusCode();
            var hit = await client.GetAsync(path);
            Assert.Equal("HIT", XPageSpeed(hit));
            Assert.Contains("0a0a0a", await hit.Content.ReadAsStringAsync());
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }

    // ── Entries recorded without the stamp ──────────────────────────

    [NativeLibTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CacheEntryWithoutAnonymousRecordStamp_IsAMiss(bool stamped)
    {
        const string path = "/planted.css";
        var app = await StartAsync(a => Css(a, path, OpenCss));
        try
        {
            byte flags = CacheFlags.HeadersClassified;
            if (stamped) flags |= CacheFlags.AnonymousRecorded;
            var planted = Encoding.UTF8.GetBytes("header{background:#feed00}");
            var cache = app.Services.GetRequiredService<IPageSpeedCache>();
            using (var writer = cache.BeginWrite(path, "localhost", "https",
                new CacheWriteParams
                {
                    AlternateId = 0x08,
                    ContentLength = (ulong)planted.Length,
                    FullMask = 0x08,
                    ContentType = PageSpeedContentType.Css,
                    Flags = flags,
                    OriginContentType = "text/css",
                }))
            {
                writer.Write(planted);
                writer.Commit();
            }

            var response = await Client(app).GetAsync(path);
            response.EnsureSuccessStatusCode();
            var body = await response.Content.ReadAsStringAsync();
            if (stamped)
            {
                // Control: the planted entry is readable and served.
                Assert.Equal("HIT", XPageSpeed(response));
                Assert.Contains("feed00", body);
                Assert.Equal(0, Served(path));
            }
            else
            {
                Assert.NotEqual("HIT", XPageSpeed(response));
                Assert.DoesNotContain("feed00", body);
                Assert.Equal(1, Served(path));
            }
        }
        finally
        {
            await app.StopAsync();
            await app.DisposeAsync();
        }
    }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Net;
using System.Security.Claims;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.Routing.Matching;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.AspNetCore.Internal;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Before a response is stored, the middleware asks the rest of the
/// pipeline again as an anonymous client and stores the response only when
/// that client gets the same status and media type.
/// </summary>
public class AnonymousFetchCheckTests
{
    private static DefaultHttpContext Answered(int status, string? contentType)
    {
        var context = new DefaultHttpContext();
        context.Request.Method = "GET";
        context.Request.Scheme = "https";
        context.Request.Host = new HostString("www.example.com", 8443);
        context.Request.PathBase = "/app";
        context.Request.Path = "/a/b.css";
        context.Request.QueryString = new QueryString("?v=2");
        context.Request.Headers.Accept = "text/css,*/*";
        context.Request.Headers.AcceptLanguage = "nl";
        context.Request.Headers.UserAgent = "Browser/1.0";
        context.Request.Headers.Cookie = "s=1";
        context.Request.Headers.Authorization = "Basic dXNlcjpwYXNz";
        context.Request.Headers["X-Custom"] = "1";
        context.Connection.RemoteIpAddress = IPAddress.Parse("192.0.2.10");
        context.Connection.LocalIpAddress = IPAddress.Parse("192.0.2.1");
        context.Connection.LocalPort = 8443;
        context.User = new ClaimsPrincipal(new ClaimsIdentity(
            new[] { new Claim(ClaimTypes.Name, "alice") }, "Test"));
        context.Response.StatusCode = status;
        context.Response.ContentType = contentType;
        return context;
    }

    [Fact]
    public async Task Check_AsksAsAnonymousClientForTheSameUrl()
    {
        var context = Answered(200, "text/css");
        HttpContext? seen = null;
        var given = await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            seen = ctx;
            ctx.Response.ContentType = "text/css; charset=utf-8";
            return Task.CompletedTask;
        }, NullLogger.Instance);

        Assert.True(given);
        Assert.NotNull(seen);
        Assert.NotSame(context, seen);
        var r = seen!.Request;
        Assert.Equal("GET", r.Method);
        Assert.Equal("https", r.Scheme);
        // The host name without the port, and no local address or port: the
        // cache key has neither.
        Assert.Equal("www.example.com", r.Host.Value);
        Assert.Equal("/app", r.PathBase.Value);
        Assert.Equal("/a/b.css", r.Path.Value);
        Assert.Equal("?v=2", r.QueryString.Value);
        Assert.Equal("text/css,*/*", r.Headers.Accept.ToString());
        Assert.Equal("nl", r.Headers.AcceptLanguage.ToString());
        Assert.Equal("Browser/1.0", r.Headers.UserAgent.ToString());
        Assert.False(r.Headers.ContainsKey("Cookie"));
        Assert.False(r.Headers.ContainsKey("Authorization"));
        Assert.False(r.Headers.ContainsKey("X-Custom"));
        Assert.Equal(IPAddress.Parse("224.0.0.0"), seen.Connection.RemoteIpAddress);
        Assert.Null(seen.Connection.LocalIpAddress);
        Assert.Equal(0, seen.Connection.LocalPort);
        Assert.DoesNotContain(seen.User.Identities, i => i.IsAuthenticated);
    }

    [Theory]
    [InlineData(403, "text/css")]
    [InlineData(401, "text/css")]
    [InlineData(302, "text/css")]
    [InlineData(200, "text/html")]
    public async Task Check_RefusesADifferentAnswer(int status, string contentType)
    {
        var context = Answered(200, "text/css");
        var given = await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            ctx.Response.StatusCode = status;
            ctx.Response.ContentType = contentType;
            return Task.CompletedTask;
        }, NullLogger.Instance);
        Assert.False(given);
    }

    [Fact]
    public async Task Check_RefusesWhenThePipelineThrows()
    {
        var context = Answered(200, "text/css");
        var given = await AnonymousFetchCheck.IsGivenAsync(context,
            _ => throw new InvalidOperationException("no"), NullLogger.Instance);
        Assert.False(given);
    }

    [Fact]
    public async Task Check_SeesStatusSetWhenTheResponseStarts()
    {
        var context = Answered(200, "text/css");
        var given = await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            ctx.Response.ContentType = "text/css";
            ctx.Response.OnStarting(() =>
            {
                ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                return Task.CompletedTask;
            });
            return Task.CompletedTask;
        }, NullLogger.Instance);
        Assert.False(given);
    }

    [Fact]
    public async Task Check_CurrentRequestReadByServicesIsTheAnonymousOne()
    {
        var services = new ServiceCollection();
        services.AddHttpContextAccessor();
        using var provider = services.BuildServiceProvider();
        var accessor = provider.GetRequiredService<IHttpContextAccessor>();

        var context = Answered(200, "text/css");
        context.RequestServices = provider;
        accessor.HttpContext = context;

        IPAddress? seenByService = null;
        var given = await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            var a = ctx.RequestServices.GetRequiredService<IHttpContextAccessor>();
            seenByService = a.HttpContext?.Connection.RemoteIpAddress;
            ctx.Response.ContentType = "text/css";
            return Task.CompletedTask;
        }, NullLogger.Instance);

        Assert.True(given);
        Assert.Equal(IPAddress.Parse("224.0.0.0"), seenByService);
        // The request being answered is still the current one afterwards.
        Assert.Same(context, accessor.HttpContext);
    }

    [Fact]
    public async Task Check_AsksForTheRequestAsItReachedTheMiddleware()
    {
        var context = Answered(200, "text/css");
        var seed = AnonymousFetchCheck.Seed.Capture(context);
        // The application rewrote the path and selected an endpoint for it.
        context.Request.Path = "/rewritten.css";
        context.SetEndpoint(new Endpoint(_ => Task.CompletedTask,
            EndpointMetadataCollection.Empty, "selected after"));

        HttpContext? seen = null;
        Assert.True(await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            seen = ctx;
            ctx.Response.ContentType = "text/css";
            return Task.CompletedTask;
        }, NullLogger.Instance, seed));
        Assert.NotNull(seen);
        Assert.Equal("/a/b.css", seen.Request.Path.Value);
        Assert.Null(seen.GetEndpoint());
    }

    [Fact]
    public async Task Check_KeepsAnEndpointSetBeforeTheMiddleware()
    {
        var context = Answered(200, "text/css");
        var endpoint = new Endpoint(_ => Task.CompletedTask,
            EndpointMetadataCollection.Empty, "selected before");
        context.SetEndpoint(endpoint);
        context.Request.RouteValues["id"] = "7";
        var seed = AnonymousFetchCheck.Seed.Capture(context);

        HttpContext? seen = null;
        Assert.True(await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            seen = ctx;
            ctx.Response.ContentType = "text/css";
            return Task.CompletedTask;
        }, NullLogger.Instance, seed));
        Assert.NotNull(seen);
        Assert.Same(endpoint, seen.GetEndpoint());
        Assert.Equal("7", seen.Request.RouteValues["id"]);
    }

    [Fact]
    public async Task Check_RefusesWhenTheVisitorsRequestIsCancelled()
    {
        using var cts = new CancellationTokenSource();
        var context = Answered(200, "text/css");
        context.RequestAborted = cts.Token;
        var started = new TaskCompletionSource();
        var check = AnonymousFetchCheck.IsGivenAsync(context, async ctx =>
        {
            ctx.Response.ContentType = "text/css";
            started.SetResult();
            await Task.Delay(Timeout.Infinite, ctx.RequestAborted);
        }, NullLogger.Instance);
        await started.Task;
        cts.Cancel();
        Assert.False(await check.WaitAsync(TimeSpan.FromSeconds(10)));
    }

    [Fact]
    public async Task Check_IsNotMadeForAnAlreadyCancelledRequest()
    {
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        var context = Answered(200, "text/css");
        context.RequestAborted = cts.Token;
        var ran = false;
        Assert.False(await AnonymousFetchCheck.IsGivenAsync(context, ctx =>
        {
            ran = true;
            ctx.Response.ContentType = "text/css";
            return Task.CompletedTask;
        }, NullLogger.Instance));
        Assert.False(ran);
    }

    // ── Endpoints selected before the middleware ────────────────────

    private sealed class AppMetadata { }

    private sealed class AppConstraint : IRouteConstraint
    {
        public bool Match(HttpContext? httpContext, IRouter? route, string routeKey,
            RouteValueDictionary values, RouteDirection routeDirection) => true;
    }

    private static IServiceProvider RoutingServices(Action<IServiceCollection>? more = null)
    {
        var services = new ServiceCollection();
        services.AddRouting(o => o.ConstraintMap["app"] = typeof(AppConstraint));
        more?.Invoke(services);
        return services.BuildServiceProvider();
    }

    private static RouteEndpoint Route(string pattern, params object[] metadata) =>
        new(_ => Task.CompletedTask,
            Microsoft.AspNetCore.Routing.Patterns.RoutePatternFactory.Parse(pattern), 0,
            new EndpointMetadataCollection(metadata), pattern);

    [Fact]
    public void EndpointDependsOnClient_FrameworkOnlyEndpointDoesNot()
    {
        var sp = RoutingServices();
        Assert.False(AnonymousFetchCheck.EndpointDependsOnClient(null, sp));
        Assert.False(AnonymousFetchCheck.EndpointDependsOnClient(
            Route("/a/{id:int}.css", new HttpMethodMetadata(new[] { "GET" })), sp));
    }

    public static IEnumerable<object[]> ClientDependentEndpoints() => new[]
    {
        new object[] { "host", Route("/a.css", new HostAttribute("*:8443")) },
        new object[] { "authorize", Route("/a.css", new Microsoft.AspNetCore.Authorization.AuthorizeAttribute()) },
        new object[] { "app-metadata", Route("/a.css", new AppMetadata()) },
        new object[] { "app-constraint", Route("/a/{id:app}.css") },
        new object[] { "unknown-constraint", Route("/a/{id:nosuch}.css") },
    };

    [Theory]
    [MemberData(nameof(ClientDependentEndpoints))]
    public void EndpointDependsOnClient_RefusesWhatItCannotRuleOut(string kind, Endpoint endpoint)
    {
        Assert.NotNull(kind);
        Assert.True(AnonymousFetchCheck.EndpointDependsOnClient(endpoint, RoutingServices()));
    }

    private sealed class ShapelessPolicy : MatcherPolicy
    {
        public override int Order => 0;
    }

    [Fact]
    public void EndpointDependsOnClient_RefusesWithoutServicesOrWithAnUnknownPolicyShape()
    {
        var endpoint = Route("/a.css");
        Assert.True(AnonymousFetchCheck.EndpointDependsOnClient(endpoint, null));
        Assert.True(AnonymousFetchCheck.EndpointDependsOnClient(endpoint,
            RoutingServices(s => s.AddSingleton<MatcherPolicy, ShapelessPolicy>())));
    }

    // ── Refusal memo ────────────────────────────────────────────────

    [Fact]
    public void RefusalMemo_RemembersARefusalForItsTtl()
    {
        long now = 1000;
        var memo = new RefusalMemo(TimeSpan.FromSeconds(60), 16, () => now);
        var key = RefusalMemo.Key("https", "www.example.com", "/a.css");
        Assert.False(memo.IsRefused(key));
        memo.Refuse(key);
        Assert.True(memo.IsRefused(key));
        Assert.False(memo.IsRefused(RefusalMemo.Key("https", "www.example.com", "/b.css")));
        Assert.False(memo.IsRefused(RefusalMemo.Key("http", "www.example.com", "/a.css")));
        now += 59_999;
        Assert.True(memo.IsRefused(key));
        now += 1;
        Assert.False(memo.IsRefused(key));
        Assert.Equal(0, memo.Count);
    }

    [Fact]
    public void RefusalMemo_StaysWithinItsSizeCap()
    {
        long now = 0;
        var memo = new RefusalMemo(TimeSpan.FromSeconds(60), 4, () => now);
        for (var i = 0; i < 100; i++)
        {
            memo.Refuse(RefusalMemo.Key("https", "h", $"/{i}.css"));
            Assert.InRange(memo.Count, 1, 4);
        }
        // The latest refusal is always kept.
        Assert.True(memo.IsRefused(RefusalMemo.Key("https", "h", "/99.css")));

        // At the cap, expired entries go first.
        var fresh = new RefusalMemo(TimeSpan.FromSeconds(1), 2, () => now);
        fresh.Refuse("old");
        now += 2000;
        fresh.Refuse("a");
        fresh.Refuse("b");
        Assert.True(fresh.IsRefused("a"));
        Assert.True(fresh.IsRefused("b"));
        Assert.Equal(2, fresh.Count);
    }

    [Theory]
    [InlineData("text/css", "TEXT/CSS; charset=utf-8", true)]
    [InlineData(null, null, true)]
    [InlineData("text/css", null, false)]
    [InlineData("text/css", "text/plain", false)]
    public void SameMediaType_IgnoresParametersAndCase(string? a, string? b, bool same) =>
        Assert.Equal(same, AnonymousFetchCheck.SameMediaType(a, b));
}

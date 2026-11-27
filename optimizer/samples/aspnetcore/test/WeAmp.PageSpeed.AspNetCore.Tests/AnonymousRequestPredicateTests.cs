// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Security.Claims;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Http;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The request-side rule for what the middleware stores: anonymous as far as
/// the application can tell from the request headers, the user and the
/// endpoint's authorization metadata.
/// </summary>
public class AnonymousRequestPredicateTests
{
    public static IEnumerable<object[]> Requests()
    {
        yield return new object[] { "plain", true };
        yield return new object[] { "authorization", false };
        yield return new object[] { "proxy-authorization", false };
        yield return new object[] { "authenticated-user", false };
        yield return new object[] { "second-identity-authenticated", false };
        yield return new object[] { "authorize-endpoint", false };
        yield return new object[] { "authorize-endpoint-allow-anonymous", true };
        yield return new object[] { "policy-endpoint", false };
        yield return new object[] { "requirement-data-endpoint", false };
        yield return new object[] { "requirement-data-endpoint-allow-anonymous", true };
        yield return new object[] { "endpoint-without-requirement", true };
    }

    [Theory]
    [MemberData(nameof(Requests))]
    public void IsAnonymousRequest_ReadsHeadersUserAndEndpoint(string kind, bool expected)
    {
        var ctx = new DefaultHttpContext();
        Endpoint MakeEndpoint(params object[] metadata) =>
            new(_ => Task.CompletedTask, new EndpointMetadataCollection(metadata), kind);
        switch (kind)
        {
            case "authorization":
                ctx.Request.Headers.Authorization = "Bearer x";
                break;
            case "proxy-authorization":
                ctx.Request.Headers["Proxy-Authorization"] = "Basic x";
                break;
            case "authenticated-user":
                ctx.User = new ClaimsPrincipal(new ClaimsIdentity("Cookies"));
                break;
            case "second-identity-authenticated":
                ctx.User = new ClaimsPrincipal(new[]
                {
                    new ClaimsIdentity(), new ClaimsIdentity("Negotiate"),
                });
                break;
            case "authorize-endpoint":
                ctx.SetEndpoint(MakeEndpoint(new AuthorizeAttribute()));
                break;
            case "authorize-endpoint-allow-anonymous":
                ctx.SetEndpoint(MakeEndpoint(
                    new AuthorizeAttribute(), new AllowAnonymousAttribute()));
                break;
            case "policy-endpoint":
                ctx.SetEndpoint(MakeEndpoint(new AuthorizationPolicyBuilder()
                    .RequireAssertion(_ => true).Build()));
                break;
            case "requirement-data-endpoint":
                ctx.SetEndpoint(MakeEndpoint(new PassRequirementAttribute()));
                break;
            case "requirement-data-endpoint-allow-anonymous":
                ctx.SetEndpoint(MakeEndpoint(
                    new PassRequirementAttribute(), new AllowAnonymousAttribute()));
                break;
            case "endpoint-without-requirement":
                ctx.SetEndpoint(MakeEndpoint());
                break;
        }
        Assert.Equal(expected, PageSpeedMiddleware.IsAnonymousRequest(ctx));
    }

    /// <summary>An endpoint requirement given as attribute metadata.</summary>
    private sealed class PassRequirementAttribute : Attribute, IAuthorizationRequirementData
    {
        public IEnumerable<IAuthorizationRequirement> GetRequirements() =>
            new IAuthorizationRequirement[]
            {
                new Microsoft.AspNetCore.Authorization.Infrastructure.AssertionRequirement(_ => true),
            };
    }
}

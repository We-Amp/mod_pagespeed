// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using FluentAssertions;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.DependencyInjection;
using WeAmp.PageSpeed.AspNetCore.DependencyInjection;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Sidecar;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The admin info endpoint exposes internal sidecar state and must require
/// authorization unless the caller explicitly opts out.
/// </summary>
public class InfoEndpointAuthorizationTests
{
    private static RouteEndpoint MapAndGet(Action<IEndpointRouteBuilder> map)
    {
        var builder = WebApplication.CreateBuilder();
        builder.Services.AddAuthorization();
        builder.Services.AddSingleton<ISidecarManager>(new FakeSidecarManager());
        builder.Services.AddSingleton(new InternalSidecarEndpoint());
        var app = builder.Build();
        map(app);
        var source = ((IEndpointRouteBuilder)app).DataSources.Single();
        return source.Endpoints.OfType<RouteEndpoint>().Single();
    }

    [Fact]
    public void MapPageSpeedInfo_RequiresAuthorizationByDefault()
    {
        var endpoint = MapAndGet(app => app.MapPageSpeedInfo());
        endpoint.Metadata.GetMetadata<IAuthorizeData>().Should().NotBeNull();
    }

    [Fact]
    public void MapPageSpeedInfo_CanOptOutOfAuthorization()
    {
        var endpoint = MapAndGet(app => app.MapPageSpeedInfo(requireAuthorization: false));
        endpoint.Metadata.GetMetadata<IAuthorizeData>().Should().BeNull();
    }
}

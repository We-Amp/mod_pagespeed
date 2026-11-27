// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Builder;
using WeAmp.PageSpeed.AspNetCore.Internal;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Extension methods for adding the PageSpeed middleware to the pipeline.
/// </summary>
public static class PageSpeedApplicationBuilderExtensions
{
    /// <summary>
    /// Adds PageSpeed HTML optimization middleware.
    /// Recommended ordering:
    ///   app.UseExceptionHandler();
    ///   app.UseHttpsRedirection();
    ///   app.UsePageSpeed();           // here
    ///   app.UseResponseCompression(); // after PageSpeed
    ///   app.UseStaticFiles();
    ///   app.UseRouting();
    /// The first thing it registers is <see cref="UsePageSpeedAgentFetchLabel"/>:
    /// middleware and endpoints after <c>UsePageSpeed()</c> see the optimizer's
    /// own agent fetches as 224.0.0.0, not as a local client. Middleware that
    /// decides by client address and must run before <c>UsePageSpeed()</c>
    /// needs <see cref="UsePageSpeedAgentFetchLabel"/> ahead of it. Behind a
    /// proxy, call <c>UseForwardedHeaders()</c> before <c>UsePageSpeed()</c>.
    /// </summary>
    public static IApplicationBuilder UsePageSpeed(
        this IApplicationBuilder app)
    {
        app.UsePageSpeedAgentFetchLabel();
        // ConsoleMiddleware reverse-proxies /v1/ws/* WebSocket upgrades
        // from the SPA to the loopback worker, which requires the
        // WebSocketMiddleware to be present in the pipeline first.
        // UseWebSockets is documented as idempotent; calling it here
        // means customer apps don't have to.
        app.UseWebSockets();
        // ConsoleMiddleware runs first so it can short-circuit /console/*
        // and the proxied /v1/* worker-API paths (health, ws, ...) before
        // any HTML optimization touches them.
        app.UseMiddleware<ConsoleMiddleware>();
        return app.UseMiddleware<PageSpeedMiddleware>();
    }

    /// <summary>
    /// Presents the optimizer's own agent fetches (the agent optimization
    /// render and the synthesized <c>/llms.txt</c>, which fetch the site's
    /// pages from this host) to every later middleware and endpoint as the
    /// client address 224.0.0.0 in
    /// <see cref="Microsoft.AspNetCore.Http.ConnectionInfo.RemoteIpAddress"/>,
    /// so a rule that admits local clients does not admit them. A request is
    /// recognized by the optimizer's User-Agent together with a client
    /// address that is loopback or the address the request arrived on; a
    /// client connecting directly from elsewhere keeps its address.
    /// <c>UsePageSpeed()</c> calls this
    /// first; call it yourself only to place it earlier, ahead of
    /// middleware that decides by client address. Behind a proxy, place it
    /// and <c>UsePageSpeed()</c> after <c>UseForwardedHeaders()</c>, so that
    /// a fetch is recognized by the forwarded client address.
    /// </summary>
    public static IApplicationBuilder UsePageSpeedAgentFetchLabel(
        this IApplicationBuilder app)
    {
        return app.UseMiddleware<AgentSelfFetchMiddleware>();
    }
}

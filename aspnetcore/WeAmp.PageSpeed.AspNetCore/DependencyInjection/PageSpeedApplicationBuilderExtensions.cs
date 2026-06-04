using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;

namespace WeAmp.PageSpeed.AspNetCore.DependencyInjection;

/// <summary>
/// Extension methods for configuring PageSpeed middleware in the request pipeline.
/// </summary>
public static class PageSpeedApplicationBuilderExtensions
{
    /// <summary>
    /// Adds the PageSpeed request-path middleware to the pipeline.
    ///
    /// <para>In <see cref="SidecarMode.Inverse"/> (the default) this activates the
    /// real <see cref="PageSpeedInverseMiddleware"/>: Kestrel is the public front
    /// door and this middleware streams optimizable requests over loopback to the
    /// bundled nginx optimize-proxy, bypassing itself for the private raw-origin
    /// leg (the loop break).</para>
    ///
    /// <para>PIPELINE PLACEMENT (Inverse): place AFTER
    /// <c>UseRouting/UseAuthentication/UseAuthorization</c> (Kestrel owns auth) and
    /// BEFORE endpoint mapping; the loop-break + hop-ceiling short-circuit private-
    /// origin traffic early regardless. Place BEFORE <c>UseForwardedHeaders</c> so
    /// the loopback transport-peer assertion reads the un-rewritten
    /// <c>Connection.RemoteIpAddress</c>.</para>
    ///
    /// <para>For Process/External/Docker this is a no-op: nginx is the front door
    /// there (the package runs the sidecar independently).</para>
    /// </summary>
    /// <param name="app">The application builder.</param>
    /// <returns>The application builder for chaining.</returns>
    public static IApplicationBuilder UsePageSpeed(this IApplicationBuilder app)
    {
        var options = app.ApplicationServices.GetRequiredService<IOptions<PageSpeedOptions>>().Value;

        if (!options.Enabled)
        {
            return app;
        }

        if (options.Sidecar.Mode == SidecarMode.Inverse)
        {
            // The real request-path middleware: Kestrel is the public front door,
            // nginx is the loopback optimize-proxy behind it.
            app.UseMiddleware<PageSpeedInverseMiddleware>();
            return app;
        }

        // Process/External/Docker: nginx is the public front door; no request-path
        // middleware is needed (the sidecar runs as a reverse proxy independently).
        return app;
    }

    /// <summary>
    /// Maps PageSpeed health check endpoint.
    /// </summary>
    /// <param name="app">The endpoint route builder.</param>
    /// <param name="pattern">The route pattern. Default: "/health/pagespeed"</param>
    /// <returns>The endpoint route builder for chaining.</returns>
    public static IEndpointRouteBuilder MapPageSpeedHealthCheck(
        this IEndpointRouteBuilder app,
        string pattern = "/health/pagespeed")
    {
        app.MapHealthChecks(pattern, new Microsoft.AspNetCore.Diagnostics.HealthChecks.HealthCheckOptions
        {
            Predicate = check => check.Tags.Contains("pagespeed")
        });

        return app;
    }

    /// <summary>
    /// Maps PageSpeed admin info endpoint (shows sidecar status and admin token).
    /// </summary>
    /// <param name="app">The endpoint route builder.</param>
    /// <param name="pattern">The route pattern. Default: "/pagespeed/info"</param>
    /// <returns>The endpoint route builder for chaining.</returns>
    public static IEndpointRouteBuilder MapPageSpeedInfo(
        this IEndpointRouteBuilder app,
        string pattern = "/pagespeed/info")
    {
        app.MapGet(pattern, (
            ISidecarManager sidecarManager,
            IOptions<PageSpeedOptions> options,
            InternalSidecarEndpoint endpoint) =>
        {
            var opts = options.Value;
            var inverse = opts.Sidecar.Mode == SidecarMode.Inverse;

            // In Inverse, nginx listens loopback-only on NginxLoopbackPort (NOT
            // Sidecar.ListenPort, which is now the public Kestrel port). The
            // admin/health URLs live behind that private port. In Process the
            // public nginx port is Sidecar.ListenPort.
            var adminPort = inverse && endpoint.NginxLoopbackPort > 0
                ? endpoint.NginxLoopbackPort
                : opts.Sidecar.ListenPort;

            return Results.Ok(new
            {
                enabled = opts.Enabled,
                mode = opts.Sidecar.Mode.ToString(),
                state = sidecarManager.State.ToString(),
                ports = new
                {
                    publicPort = opts.Sidecar.ListenPort,
                    nginxLoopback = inverse ? endpoint.NginxLoopbackPort : 0,
                    rawOrigin = inverse ? endpoint.RawOriginLoopbackPort : opts.Sidecar.OriginPort
                },
                allowPublicAdmin = inverse ? opts.Sidecar.AllowPublicAdmin : (bool?)null,
                adminEndpoints = new
                {
                    health = $"http://localhost:{adminPort}/pagespeed/health",
                    admin = $"http://localhost:{adminPort}/pagespeed_admin",
                    statistics = $"http://localhost:{adminPort}/pagespeed_statistics"
                },
                restartCount = sidecarManager.RestartCount,
                error = sidecarManager.ErrorMessage
            });
        }).WithTags("PageSpeed").WithName("GetPageSpeedInfo");

        return app;
    }
}

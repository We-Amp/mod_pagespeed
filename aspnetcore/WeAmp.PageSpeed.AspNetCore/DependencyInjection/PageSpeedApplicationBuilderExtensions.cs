using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;

namespace WeAmp.PageSpeed.AspNetCore.DependencyInjection;

/// <summary>
/// Extension methods for configuring PageSpeed middleware in the request pipeline.
/// </summary>
public static class PageSpeedApplicationBuilderExtensions
{
    /// <summary>
    /// Adds PageSpeed middleware to the request pipeline.
    /// This is optional - the sidecar runs independently.
    /// Currently a no-op but reserved for future request interception features.
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

        // Currently no middleware needed - the sidecar operates as a reverse proxy
        // This extension point is reserved for future features like:
        // - Request routing based on ExcludePaths
        // - Response header inspection
        // - Metrics collection

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
            IOptions<PageSpeedOptions> options) =>
        {
            var opts = options.Value;

            return Results.Ok(new
            {
                enabled = opts.Enabled,
                state = sidecarManager.State.ToString(),
                ports = new
                {
                    listen = opts.Sidecar.ListenPort,
                    origin = opts.Sidecar.OriginPort,
                    admin = opts.Sidecar.AdminPort
                },
                adminEndpoints = new
                {
                    health = $"http://localhost:{opts.Sidecar.ListenPort}/pagespeed/health",
                    admin = $"http://localhost:{opts.Sidecar.ListenPort}/pagespeed_admin",
                    statistics = $"http://localhost:{opts.Sidecar.ListenPort}/pagespeed_statistics"
                },
                restartCount = sidecarManager.RestartCount,
                error = sidecarManager.ErrorMessage
            });
        }).WithTags("PageSpeed").WithName("GetPageSpeedInfo");

        return app;
    }
}

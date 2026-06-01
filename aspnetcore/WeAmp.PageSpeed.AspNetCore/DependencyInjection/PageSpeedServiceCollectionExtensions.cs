using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.HealthChecks;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;

namespace WeAmp.PageSpeed.AspNetCore.DependencyInjection;

/// <summary>
/// Extension methods for adding PageSpeed services to the DI container.
/// </summary>
public static class PageSpeedServiceCollectionExtensions
{
    /// <summary>
    /// Adds PageSpeed optimization middleware with default configuration.
    /// Uses Process sidecar mode (spawns envoy_pagespeed as a child process).
    /// </summary>
    /// <param name="services">The service collection.</param>
    /// <returns>The service collection for chaining.</returns>
    public static IServiceCollection AddPageSpeed(this IServiceCollection services)
    {
        return services.AddPageSpeed(_ => { });
    }

    /// <summary>
    /// Adds PageSpeed optimization middleware with custom configuration.
    /// Uses Process sidecar mode (spawns envoy_pagespeed as a child process).
    /// </summary>
    /// <param name="services">The service collection.</param>
    /// <param name="configure">Configuration action.</param>
    /// <returns>The service collection for chaining.</returns>
    public static IServiceCollection AddPageSpeed(
        this IServiceCollection services,
        Action<PageSpeedOptions> configure)
    {
        services.AddOptions<PageSpeedOptions>()
            .Configure(configure)
            .ValidateOnStart();

        return services.AddPageSpeedCore<ProcessSidecarManager>();
    }

    /// <summary>
    /// Adds PageSpeed optimization middleware with configuration from IConfiguration.
    /// Uses Process sidecar mode (spawns envoy_pagespeed as a child process).
    /// </summary>
    /// <param name="services">The service collection.</param>
    /// <param name="configuration">The configuration section.</param>
    /// <returns>The service collection for chaining.</returns>
    public static IServiceCollection AddPageSpeed(
        this IServiceCollection services,
        IConfiguration configuration)
    {
        services.AddOptions<PageSpeedOptions>()
            .Bind(configuration.GetSection(PageSpeedOptions.SectionName))
            .ValidateOnStart();

        return services.AddPageSpeedCore<ProcessSidecarManager>();
    }

    /// <summary>
    /// Adds PageSpeed optimization middleware for connecting to an externally-managed Envoy sidecar.
    /// Use this mode when Envoy is managed by Kubernetes or another orchestrator.
    /// </summary>
    /// <param name="services">The service collection.</param>
    /// <param name="configure">Configuration action.</param>
    /// <returns>The service collection for chaining.</returns>
    public static IServiceCollection AddPageSpeedExternal(
        this IServiceCollection services,
        Action<PageSpeedOptions>? configure = null)
    {
        services.AddOptions<PageSpeedOptions>()
            .Configure(opts =>
            {
                opts.Sidecar.Mode = SidecarMode.External;
                configure?.Invoke(opts);
            })
            .ValidateOnStart();

        return services.AddPageSpeedCore<ProcessSidecarManager>();
    }

    private static IServiceCollection AddPageSpeedCore<TSidecarManager>(this IServiceCollection services)
        where TSidecarManager : class, ISidecarManager
    {
        // Add HTTP client factory for health checks
        services.AddHttpClient();

        // Add core services
        services.TryAddSingleton<EnvoyConfigGenerator>();
        services.TryAddSingleton<ISidecarManager, TSidecarManager>();

        // Add hosted service for lifecycle management
        services.AddHostedService<PageSpeedSidecarHostedService>();

        // Add health check
        services.AddHealthChecks()
            .AddCheck<PageSpeedHealthCheck>("pagespeed", tags: ["pagespeed", "sidecar"]);

        return services;
    }
}

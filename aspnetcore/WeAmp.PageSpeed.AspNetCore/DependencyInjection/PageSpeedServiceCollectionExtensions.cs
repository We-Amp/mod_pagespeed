// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Config;
using WeAmp.PageSpeed.AspNetCore.HealthChecks;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;

namespace WeAmp.PageSpeed.AspNetCore.DependencyInjection;

/// <summary>
/// Extension methods for adding PageSpeed services to the DI container.
/// </summary>
public static class PageSpeedServiceCollectionExtensions
{
    /// <summary>
    /// Adds PageSpeed optimization with default configuration. Defaults to the
    /// Inverse topology (Kestrel is the public front door; the bundled nginx runs
    /// loopback-only behind it as an optimize-proxy). Use
    /// <see cref="AddPageSpeedProcess"/> for the classic front-proxy topology.
    /// </summary>
    /// <param name="services">The service collection.</param>
    /// <returns>The service collection for chaining.</returns>
    public static IServiceCollection AddPageSpeed(this IServiceCollection services)
    {
        return services.AddPageSpeed(_ => { });
    }

    /// <summary>
    /// Adds PageSpeed optimization with custom configuration. The mode flows from
    /// <see cref="SidecarOptions.Mode"/> (default <see cref="SidecarMode.Inverse"/>).
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
    /// Adds PageSpeed optimization with configuration from IConfiguration. The mode
    /// flows from <c>PageSpeed:Sidecar:Mode</c> (default
    /// <see cref="SidecarMode.Inverse"/>).
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
    /// Adds PageSpeed optimization in the Inverse topology (convenience; mirrors
    /// <see cref="AddPageSpeedExternal"/>). Kestrel is the public front door and
    /// the bundled nginx runs loopback-only behind it. Equivalent to
    /// <c>AddPageSpeed(o =&gt; { o.Sidecar.Mode = SidecarMode.Inverse; ... })</c>.
    /// </summary>
    public static IServiceCollection AddPageSpeedInverse(
        this IServiceCollection services,
        Action<PageSpeedOptions>? configure = null)
    {
        services.AddOptions<PageSpeedOptions>()
            .Configure(opts =>
            {
                opts.Sidecar.Mode = SidecarMode.Inverse;
                configure?.Invoke(opts);
            })
            .ValidateOnStart();

        return services.AddPageSpeedCore<ProcessSidecarManager>();
    }

    /// <summary>
    /// Adds PageSpeed optimization in the classic front-proxy topology (Process):
    /// the bundled nginx is the PUBLIC front door, Kestrel is the private origin.
    /// The explicit escape hatch from the new Inverse default.
    /// </summary>
    public static IServiceCollection AddPageSpeedProcess(
        this IServiceCollection services,
        Action<PageSpeedOptions>? configure = null)
    {
        services.AddOptions<PageSpeedOptions>()
            .Configure(opts =>
            {
                opts.Sidecar.Mode = SidecarMode.Process;
                configure?.Invoke(opts);
            })
            .ValidateOnStart();

        return services.AddPageSpeedCore<ProcessSidecarManager>();
    }

    /// <summary>
    /// Adds PageSpeed optimization for connecting to an externally-managed nginx
    /// reverse proxy (operator-run; the package does not spawn or manage it).
    /// The package does not model orchestrated multi-replica topologies: it points
    /// at one operator-run nginx and coordinates nothing across replicas.
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

        // Validate options eagerly at host build. The .ValidateOnStart() calls in the
        // public AddPageSpeed* overloads are a no-op without a registered validator;
        // this rejects the unsupported Docker mode and out-of-range/colliding ports
        // with a clear message instead of an opaque nginx startup crash.
        services.TryAddEnumerable(
            ServiceDescriptor.Singleton<IValidateOptions<PageSpeedOptions>, PageSpeedOptionsValidator>());

        // Add core services
        services.TryAddSingleton<NginxConfigGenerator>();
        // Rendezvous singleton: the single source of truth for the Kestrel origin
        // that both the Kestrel configurator and the nginx config generator read.
        services.TryAddSingleton<InternalSidecarEndpoint>();
        services.TryAddSingleton<ISidecarManager, TSidecarManager>();

        // Pin Kestrel to the internal sidecar RAW-ORIGIN (UDS or loopback in
        // Process; forced loopback-TCP + marker tag in Inverse) and publish it
        // into InternalSidecarEndpoint so the generated nginx proxy_pass and the
        // Kestrel bind never disagree. No-op for Docker/External modes, where the
        // package does not own the Kestrel bind.
        services.TryAddEnumerable(
            ServiceDescriptor.Singleton<IConfigureOptions<KestrelServerOptions>, SidecarKestrelConfigureOptions>());

        // INVERSE public-bind, kept SEPARATE from the raw-origin configurator so a
        // bug in one cannot widen the other (review fix). Auto-binds the public
        // port only when Sidecar.OwnPublicPort is set; otherwise the operator owns
        // the public bind. No-op outside Inverse.
        services.TryAddEnumerable(
            ServiceDescriptor.Singleton<IConfigureOptions<KestrelServerOptions>, PublicKestrelConfigureOptions>());

        // INVERSE forward path: YARP's IHttpForwarder + a singleton loopback
        // HttpMessageInvoker + the InverseForwardTransformer behind the
        // IPageSpeedForwarder testability seam. Streaming, no buffering.
        services.AddHttpForwarder();
        services.TryAddSingleton(sp =>
        {
            var opts = sp.GetRequiredService<IOptions<PageSpeedOptions>>().Value;
            return new LoopbackHttpMessageInvoker(
                TimeSpan.FromMilliseconds(Math.Max(1, opts.Sidecar.HealthCheckTimeoutMs)));
        });
        services.TryAddSingleton<InverseForwardTransformer>();
        services.TryAddSingleton<IPageSpeedForwarder, HttpForwarderAdapter>();

        // Add hosted service for lifecycle management
        services.AddHostedService<PageSpeedSidecarHostedService>();

        // Add health check
        services.AddHealthChecks()
            .AddCheck<PageSpeedHealthCheck>("pagespeed", tags: ["pagespeed", "sidecar"]);

        return services;
    }
}

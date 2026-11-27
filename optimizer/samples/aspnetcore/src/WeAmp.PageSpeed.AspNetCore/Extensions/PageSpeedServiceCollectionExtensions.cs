// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Extension methods for registering PageSpeed services in DI.
/// </summary>
public static class PageSpeedServiceCollectionExtensions
{
    /// <summary>
    /// Adds PageSpeed services to the DI container: cache (singleton),
    /// HTML processor, worker notification BackgroundService, and health check.
    /// </summary>
    /// <param name="services">The service collection.</param>
    /// <param name="configure">Optional callback to configure options.</param>
    public static IServiceCollection AddPageSpeed(
        this IServiceCollection services,
        Action<PageSpeedOptions>? configure = null)
    {
        // BindConfiguration is reached via the source-generated binder
        // (EnableConfigurationBindingGenerator=true in the csproj), so it
        // does not trigger IL2026/IL3050 under trimming/AOT.
        //
        // ValidateDataAnnotations() is intentionally NOT called: it reflects
        // over TOptions to find [Required]/[Range]/etc. attributes, which
        // is unsafe under trimming (IL2026). The equivalent constraints
        // on PageSpeedCacheOptions are enforced in code by
        // PageSpeedOptionsValidator (registered below).
        var builder = services.AddOptions<PageSpeedOptions>()
            .BindConfiguration(PageSpeedOptions.SectionName);
        if (configure != null)
            builder.Configure(configure);
        builder.ValidateOnStart();

        services.AddSingleton<IValidateOptions<PageSpeedOptions>,
            PageSpeedOptionsValidator>();

        // Runtime version check: fail fast if native library is incompatible.
        VersionCheck.EnsureCompatible();

        // Activation: the one point where PageSpeed becomes active, the first
        // time PageSpeed:Enabled is true (at startup or through a hot reload).
        // It opens the real cache, then releases WorkerProcessHost to launch
        // the worker. Until then nothing touches the disk. Enabled going back
        // to false tears nothing down.
        services.AddSingleton(sp =>
        {
            var loggerFactory = sp.GetService<ILoggerFactory>();
            return new PageSpeedActivation(
                sp.GetRequiredService<IOptionsMonitor<PageSpeedOptions>>(),
                opts => OpenCache(opts, loggerFactory),
                loggerFactory?.CreateLogger<PageSpeedActivation>());
        });

        // Cache: singleton, disposed on shutdown. Registered as interface.
        // An empty stand-in until activation, then the real cache.
        services.AddSingleton<IPageSpeedCache>(sp =>
            sp.GetRequiredService<PageSpeedActivation>().Cache);

        // HTML processor: singleton, reads IOptionsMonitor per call.
        services.AddSingleton<IHtmlProcessor, HtmlProcessor>();

        // Worker notification: bounded channel + BackgroundService.
        services.AddSingleton<WorkerNotificationService>();
        services.AddHostedService(sp =>
            sp.GetRequiredService<WorkerNotificationService>());

        // Internal worker endpoint: shared rendezvous for the loopback
        // port that WorkerProcessHost allocates and ConsoleMiddleware
        // reads.
        services.AddSingleton<InternalWorkerEndpoint>();

        // Worker process host: auto-starts factory_worker as a child process.
        // It waits for the activation above, so a host that starts with
        // PageSpeed disabled launches the worker once a hot reload enables it.
        services.AddSingleton(sp => new WorkerProcessHost(
            sp.GetRequiredService<PageSpeedActivation>(),
            sp.GetRequiredService<IOptionsMonitor<PageSpeedOptions>>(),
            sp.GetRequiredService<InternalWorkerEndpoint>(),
            sp.GetRequiredService<ILogger<WorkerProcessHost>>()));
        services.AddHostedService(sp =>
            sp.GetRequiredService<WorkerProcessHost>());

        // Console proxy: the named HttpClient used by ConsoleMiddleware
        // to forward the console's /v1/* API calls to the loopback worker.
        // BaseAddress is intentionally not set here — the middleware
        // constructs the absolute URI per request once the endpoint port
        // is published, which avoids racing with the worker startup.
        services.AddHttpClient("PageSpeedConsoleProxy", client =>
        {
            client.Timeout = TimeSpan.FromSeconds(5);
        })
        .ConfigurePrimaryHttpMessageHandler(LoopbackConnect.CreateHandler);

        // Health check.
        services.AddHealthChecks()
            .AddCheck<PageSpeedHealthCheck>("pagespeed",
                tags: ["pagespeed", "ready"]);

        return services;
    }

    /// <summary>
    /// Opens the real cache on activation. Wrapped so it follows the worker's
    /// volume resets (full purge): the wrapper reopens the cache when
    /// {VolumePath}.gen changes.
    /// </summary>
    private static IPageSpeedCache OpenCache(
        PageSpeedOptions opts, ILoggerFactory? loggerFactory)
    {
        var cacheOpts = new WeAmp.PageSpeed.PageSpeedCacheOptions
        {
            VolumePath = opts.Cache.VolumePath,
            VolumeSizeBytes = opts.Cache.VolumeSizeBytes,
            EnableChecksum = opts.Cache.EnableChecksum,
            RamCacheSizeBytes = opts.Cache.RamCacheSizeBytes,
            MaxMetadataSizeBytes = opts.Cache.MaxMetadataSizeBytes,
        };
        var logger = loggerFactory?.CreateLogger<GenerationAwarePageSpeedCache>();
        return new GenerationAwarePageSpeedCache(
            () => new PageSpeedCache(cacheOpts),
            cacheOpts.VolumePath + ".gen",
            logger);
    }
}

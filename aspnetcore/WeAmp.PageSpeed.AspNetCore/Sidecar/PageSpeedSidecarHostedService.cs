// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Sidecar;

/// <summary>
/// Hosted service that manages the PageSpeed sidecar lifecycle.
/// </summary>
public class PageSpeedSidecarHostedService : IHostedLifecycleService, IDisposable
{
    private readonly ILogger<PageSpeedSidecarHostedService> _logger;
    private readonly ISidecarManager _sidecarManager;
    private readonly IOptions<PageSpeedOptions> _options;
    private readonly IHostApplicationLifetime _applicationLifetime;

    public PageSpeedSidecarHostedService(
        ILogger<PageSpeedSidecarHostedService> logger,
        ISidecarManager sidecarManager,
        IOptions<PageSpeedOptions> options,
        IHostApplicationLifetime applicationLifetime)
    {
        _logger = logger;
        _sidecarManager = sidecarManager;
        _options = options;
        _applicationLifetime = applicationLifetime;

        _sidecarManager.StateChanged += OnSidecarStateChanged;
    }

    public Task StartingAsync(CancellationToken cancellationToken)
    {
        _logger.LogDebug("PageSpeed sidecar service starting");
        return Task.CompletedTask;
    }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        if (!_options.Value.Enabled)
        {
            _logger.LogInformation("PageSpeed is disabled");
            return;
        }

        // Fail-fast: a sidecar that cannot start is a hard error — do
        // NOT swallow it and let the app come up without optimization. The manager
        // config-tests the generated nginx.conf (nginx -t, captured stderr) and waits
        // for the health endpoint before reporting Running, so an exception here is
        // genuinely fatal.
        await _sidecarManager.StartAsync(cancellationToken);
    }

    public Task StartedAsync(CancellationToken cancellationToken)
    {
        if (_options.Value.Enabled && _sidecarManager.State == SidecarState.Running)
        {
            var opts = _options.Value;
            _logger.LogInformation(
                "PageSpeed optimization active. Requests on port {ListenPort} are optimized and forwarded to port {OriginPort}",
                opts.Sidecar.ListenPort,
                opts.Sidecar.OriginPort);

            if (_sidecarManager.AdminToken != null)
            {
                // Never log the token value — it is a bearer credential and logs
                // are shipped/retained/broadly readable. Retrieve it
                // programmatically via ISidecarManager.AdminToken instead.
                _logger.LogInformation(
                    "Admin endpoints require a bearer token ({Length}-char credential). " +
                    "Retrieve it via ISidecarManager.AdminToken; do not log it.",
                    _sidecarManager.AdminToken.Length);
            }
        }

        return Task.CompletedTask;
    }

    public Task StoppingAsync(CancellationToken cancellationToken)
    {
        _logger.LogDebug("PageSpeed sidecar service stopping");
        return Task.CompletedTask;
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_sidecarManager.State is SidecarState.Running or SidecarState.Unhealthy)
        {
            await _sidecarManager.StopAsync(cancellationToken);
        }
    }

    public Task StoppedAsync(CancellationToken cancellationToken)
    {
        _logger.LogDebug("PageSpeed sidecar service stopped");
        return Task.CompletedTask;
    }

    private void OnSidecarStateChanged(object? sender, SidecarStateChangedEventArgs e)
    {
        _logger.LogDebug("Sidecar state changed: {Previous} -> {New}",
            e.PreviousState, e.NewState);

        if (e.NewState == SidecarState.Failed)
        {
            _logger.LogError("PageSpeed sidecar failed: {Error}", e.ErrorMessage);
        }
    }

    public void Dispose()
    {
        _sidecarManager.StateChanged -= OnSidecarStateChanged;

        if (_sidecarManager is IDisposable disposable)
        {
            disposable.Dispose();
        }
    }
}

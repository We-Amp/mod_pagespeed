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

        try
        {
            await _sidecarManager.StartAsync(cancellationToken);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Failed to start PageSpeed sidecar");
            // Don't prevent app startup, but log the error
        }
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
                _logger.LogInformation(
                    "Admin token: {Token} (use with 'Authorization: Bearer <token>' header)",
                    _sidecarManager.AdminToken);
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

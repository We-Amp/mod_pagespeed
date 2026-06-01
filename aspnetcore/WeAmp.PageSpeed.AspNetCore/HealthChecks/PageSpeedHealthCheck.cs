using Microsoft.Extensions.Diagnostics.HealthChecks;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Options;
using WeAmp.PageSpeed.AspNetCore.Sidecar;

namespace WeAmp.PageSpeed.AspNetCore.HealthChecks;

/// <summary>
/// Health check for the PageSpeed sidecar.
/// </summary>
public class PageSpeedHealthCheck : IHealthCheck
{
    private readonly ISidecarManager _sidecarManager;
    private readonly IOptions<PageSpeedOptions> _options;

    public PageSpeedHealthCheck(
        ISidecarManager sidecarManager,
        IOptions<PageSpeedOptions> options)
    {
        _sidecarManager = sidecarManager;
        _options = options;
    }

    public async Task<HealthCheckResult> CheckHealthAsync(
        HealthCheckContext context,
        CancellationToken cancellationToken = default)
    {
        var opts = _options.Value;

        if (!opts.Enabled)
        {
            return HealthCheckResult.Healthy("PageSpeed is disabled");
        }

        var state = _sidecarManager.State;

        return state switch
        {
            SidecarState.NotStarted =>
                HealthCheckResult.Degraded("PageSpeed sidecar has not started"),

            SidecarState.Starting =>
                HealthCheckResult.Degraded("PageSpeed sidecar is starting"),

            SidecarState.Running =>
                await CheckSidecarHealthAsync(cancellationToken),

            SidecarState.Unhealthy =>
                HealthCheckResult.Unhealthy("PageSpeed sidecar is unhealthy"),

            SidecarState.Restarting =>
                HealthCheckResult.Degraded($"PageSpeed sidecar is restarting (attempt {_sidecarManager.RestartCount})"),

            SidecarState.Stopped =>
                HealthCheckResult.Unhealthy("PageSpeed sidecar has stopped"),

            SidecarState.Failed =>
                HealthCheckResult.Unhealthy($"PageSpeed sidecar failed: {_sidecarManager.ErrorMessage}"),

            _ => HealthCheckResult.Unhealthy($"Unknown sidecar state: {state}")
        };
    }

    private async Task<HealthCheckResult> CheckSidecarHealthAsync(CancellationToken cancellationToken)
    {
        try
        {
            var healthy = await _sidecarManager.CheckHealthAsync(cancellationToken);

            if (healthy)
            {
                var data = new Dictionary<string, object>
                {
                    ["state"] = _sidecarManager.State.ToString(),
                    ["listenPort"] = _options.Value.Sidecar.ListenPort,
                    ["originPort"] = _options.Value.Sidecar.OriginPort
                };

                return HealthCheckResult.Healthy("PageSpeed sidecar is healthy", data);
            }

            return HealthCheckResult.Unhealthy("PageSpeed sidecar health check failed");
        }
        catch (Exception ex)
        {
            return HealthCheckResult.Unhealthy("PageSpeed sidecar health check error", ex);
        }
    }
}

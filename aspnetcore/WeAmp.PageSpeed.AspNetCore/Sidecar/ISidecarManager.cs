namespace WeAmp.PageSpeed.AspNetCore.Sidecar;

/// <summary>
/// Manages the PageSpeed nginx sidecar lifecycle.
/// </summary>
public interface ISidecarManager
{
    /// <summary>
    /// Current state of the sidecar.
    /// </summary>
    SidecarState State { get; }

    /// <summary>
    /// Admin token for authenticating admin requests.
    /// Available after the sidecar is started.
    /// </summary>
    string? AdminToken { get; }

    /// <summary>
    /// Error message if the sidecar failed to start.
    /// </summary>
    string? ErrorMessage { get; }

    /// <summary>
    /// Number of restart attempts since last successful start.
    /// </summary>
    int RestartCount { get; }

    /// <summary>
    /// Starts the sidecar process.
    /// </summary>
    /// <param name="cancellationToken">Cancellation token.</param>
    Task StartAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Stops the sidecar process gracefully.
    /// </summary>
    /// <param name="cancellationToken">Cancellation token.</param>
    Task StopAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Checks if the sidecar is healthy.
    /// </summary>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>True if healthy, false otherwise.</returns>
    Task<bool> CheckHealthAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Event raised when the sidecar state changes.
    /// </summary>
    event EventHandler<SidecarStateChangedEventArgs>? StateChanged;
}

/// <summary>
/// Event arguments for sidecar state changes.
/// </summary>
public class SidecarStateChangedEventArgs : EventArgs
{
    /// <summary>
    /// Previous state.
    /// </summary>
    public SidecarState PreviousState { get; }

    /// <summary>
    /// New state.
    /// </summary>
    public SidecarState NewState { get; }

    /// <summary>
    /// Error message if transitioning to Failed state.
    /// </summary>
    public string? ErrorMessage { get; }

    public SidecarStateChangedEventArgs(SidecarState previousState, SidecarState newState, string? errorMessage = null)
    {
        PreviousState = previousState;
        NewState = newState;
        ErrorMessage = errorMessage;
    }
}

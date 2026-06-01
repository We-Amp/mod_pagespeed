namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Sidecar mode for PageSpeed Envoy process management.
/// </summary>
public enum SidecarMode
{
    /// <summary>
    /// Spawn envoy_pagespeed as a child process (default).
    /// </summary>
    Process,

    /// <summary>
    /// Manage envoy_pagespeed via Docker container (future).
    /// </summary>
    Docker,

    /// <summary>
    /// Connect to externally-managed Envoy (e.g., Kubernetes sidecar).
    /// </summary>
    External
}

/// <summary>
/// Configuration for the PageSpeed sidecar process.
/// </summary>
public class SidecarOptions
{
    /// <summary>
    /// Sidecar mode: Process, Docker, or External. Default: Process.
    /// </summary>
    public SidecarMode Mode { get; set; } = SidecarMode.Process;

    /// <summary>
    /// Port that Envoy listens on (receives requests from clients).
    /// Default: 8080.
    /// </summary>
    public int ListenPort { get; set; } = 8080;

    /// <summary>
    /// Port that Kestrel listens on (Envoy forwards requests here).
    /// Default: 5000.
    /// </summary>
    public int OriginPort { get; set; } = 5000;

    /// <summary>
    /// Envoy admin interface port. Default: 9901.
    /// </summary>
    public int AdminPort { get; set; } = 9901;

    /// <summary>
    /// Address to bind the admin interface to. Default: "0.0.0.0" (all interfaces).
    /// Set to "127.0.0.1" to restrict to localhost only.
    /// </summary>
    public string AdminBindAddress { get; set; } = "0.0.0.0";

    /// <summary>
    /// Path to envoy_pagespeed binary. If not set, searches PATH and well-known locations.
    /// </summary>
    public string? BinaryPath { get; set; }

    /// <summary>
    /// Directory for generated configuration files.
    /// Default: system temp directory + "pagespeed_sidecar".
    /// </summary>
    public string? ConfigDirectory { get; set; }

    /// <summary>
    /// Whether to automatically restart the sidecar on crash. Default: true.
    /// </summary>
    public bool AutoRestart { get; set; } = true;

    /// <summary>
    /// Maximum number of restart attempts before giving up. Default: 5.
    /// </summary>
    public int MaxRestartAttempts { get; set; } = 5;

    /// <summary>
    /// Delay between restart attempts in milliseconds. Default: 1000.
    /// </summary>
    public int RestartDelayMs { get; set; } = 1000;

    /// <summary>
    /// Health check poll interval in milliseconds. Default: 5000.
    /// </summary>
    public int HealthCheckIntervalMs { get; set; } = 5000;

    /// <summary>
    /// Timeout for health check requests in milliseconds. Default: 2000.
    /// </summary>
    public int HealthCheckTimeoutMs { get; set; } = 2000;

    /// <summary>
    /// Startup timeout in milliseconds. Default: 30000.
    /// </summary>
    public int StartupTimeoutMs { get; set; } = 30000;

    /// <summary>
    /// Graceful shutdown timeout in milliseconds. Default: 10000.
    /// </summary>
    public int ShutdownTimeoutMs { get; set; } = 10000;

    /// <summary>
    /// Additional command-line arguments to pass to envoy_pagespeed.
    /// </summary>
    public List<string> AdditionalArguments { get; set; } = [];

    /// <summary>
    /// Environment variables to set for the sidecar process.
    /// </summary>
    public Dictionary<string, string> EnvironmentVariables { get; set; } = [];
}

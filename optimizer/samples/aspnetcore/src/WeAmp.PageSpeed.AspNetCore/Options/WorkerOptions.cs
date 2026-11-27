// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Configuration for worker process management and notification.
/// </summary>
public sealed class WorkerOptions
{
    /// <summary>
    /// Automatically start and manage the factory_worker process.
    /// When true, the worker binary is resolved from the NuGet package
    /// and launched as a child process, but only while
    /// <see cref="PageSpeedOptions.Enabled"/> is also true. When that switch
    /// is false at startup, the child is launched once a hot reload sets it
    /// to true; switching it off later leaves a running child running.
    /// Set false to run no child worker (for example an external worker).
    /// Default: true.
    /// </summary>
    public bool AutoStart { get; set; } = true;

    // Backing state for SocketPath. We must distinguish three cases:
    //
    //   1. Never assigned  → derive an auto-path when AutoStart is true so the
    //      worker's notification IPC actually works out of the box. This is the
    //      v2.0.14 fix: the historical default of null silently disabled
    //      notifications, so notifications.received / variants.written stayed 0
    //      and the Dashboard / Savings / Metrics pages showed zeros forever.
    //   2. Assigned a non-null path → honor it verbatim.
    //   3. Assigned null explicitly (in code OR via "SocketPath": null in
    //      configuration) → coordination disabled. This is the documented
    //      escape hatch for genuinely worker-less hosts.
    //
    // The resolved auto-path is memoized per instance. Note that IOptions and
    // IOptionsMonitor hand out DISTINCT WorkerOptions instances, so the worker
    // host and the notification service do NOT share this memoized value — the
    // host pins the value it launches the worker with into the shared
    // InternalWorkerEndpoint, and the notification service reads it back from
    // there. The memoization here only guarantees the value is stable across
    // repeated reads on a single instance.
    private readonly object _socketPathGate = new();
    private string? _socketPath;
    private bool _explicitlySet;
    private string? _resolvedAutoPath;

    /// <summary>
    /// Path to the worker's IPC endpoint (Unix domain socket on Linux/macOS,
    /// named pipe on Windows).
    /// <para>
    /// When never assigned and <see cref="AutoStart"/> is <c>true</c>, this
    /// resolves to a stable per-process temp path so worker notifications work
    /// out of the box. When assigned <c>null</c> explicitly — in code or via
    /// <c>"SocketPath": null</c> in configuration — worker coordination is
    /// disabled (no notifications are sent and, if you also leave
    /// <see cref="AutoStart"/> at its default, the auto-launched worker has no
    /// socket to bind). Assign a concrete path to pin a specific endpoint
    /// (e.g. a shared socket for an out-of-process worker container).
    /// </para>
    /// <para>
    /// The auto-path is resolved once and memoized per instance. The worker
    /// host pins the value it launches the worker with into the shared
    /// <c>InternalWorkerEndpoint</c>, and the notification service reads it
    /// back from there — they do not rely on observing the same options
    /// instance.
    /// </para>
    /// </summary>
    public string? SocketPath
    {
        get
        {
            // Fast path: explicit assignment wins, no locking needed once set.
            if (_explicitlySet)
                return _socketPath;
            if (!AutoStart)
                return null;
            // Memoize the auto-path under a lock so two concurrent first-reads
            // (WorkerProcessHost vs WorkerNotificationService) can't each mint
            // a different GUID-suffixed path.
            lock (_socketPathGate)
            {
                if (_explicitlySet)
                    return _socketPath;
                return _resolvedAutoPath ??= BuildAutoPath();
            }
        }
        set
        {
            lock (_socketPathGate)
            {
                _explicitlySet = true;
                // Normalize empty / whitespace to null = disabled. This matters
                // for cross-framework config binding: the source-generated
                // binder represents a JSON `"SocketPath": null` as an empty
                // string on net8.0 (and as null on net10.0). Either way the
                // operator's intent is "disable coordination", and "" is never
                // a usable socket path, so we collapse it to null here.
                _socketPath = string.IsNullOrWhiteSpace(value) ? null : value;
            }
        }
    }

    private static string BuildAutoPath()
    {
        // Per-process, per-instance uniqueness so multiple AddPageSpeed hosts
        // in one process (e.g. test runners) don't collide on the same socket.
        // Kept short: macOS caps sun_path at 104 bytes, so we avoid
        // Path.GetTempPath() on macOS (it returns a long /var/folders/.../T/
        // path) and use /tmp directly there.
        var pid = Environment.ProcessId;
        var token = Guid.NewGuid().ToString("N").Substring(0, 8);
        var fileName = $"pagespeed-{pid}-{token}.sock";

        var dir = OperatingSystem.IsMacOS()
            ? "/tmp"
            : Path.GetTempPath();
        return Path.Combine(dir, fileName);
    }

    /// <summary>
    /// Loopback port for the worker's HTTP management API. Default: 0.
    /// 0 does not disable the API: the host picks a free loopback port.
    /// The worker is always started with <c>--api-no-auth</c>, so its API
    /// has no authentication on either port, and the middleware's console
    /// proxy (<see cref="ConsoleOptions"/>) serves its <c>/v1/*</c> paths
    /// from the app's own port. Never publish this port; turn the console
    /// off or gate it to keep the API private.
    /// </summary>
    public int ApiPort { get; set; }

    /// <summary>
    /// Worker log level: debug, info, warning, error.
    /// Null uses the worker's default (info).
    /// </summary>
    public string? LogLevel { get; set; }

    /// <summary>
    /// Path to the web console SPA directory. When null (default) and AutoStart
    /// is true, auto-discovers a 'console' directory next to factory_worker.
    /// Set to empty string to stop the worker serving its own loopback copy
    /// of the console. This does not affect the middleware's /console/; use
    /// <see cref="ConsoleOptions.Enabled"/> for that.
    /// </summary>
    public string? ConsoleDir { get; set; }

    /// <summary>
    /// No longer used; the setting is ignored. Kept so existing configuration
    /// keeps binding.
    /// </summary>
    [Obsolete("No longer used; the setting is ignored.")]
    public string LicenseRenewalUrl { get; set; } = "";
}

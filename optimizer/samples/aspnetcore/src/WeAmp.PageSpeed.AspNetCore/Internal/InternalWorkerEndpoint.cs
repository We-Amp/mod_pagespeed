// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Singleton holding the rendezvous coordinates for the factory_worker:
/// the loopback port it listens on and the IPC socket/pipe path it binds.
/// Written once by <see cref="WorkerProcessHost"/> just before the worker
/// child process is spawned; the port is read by
/// <see cref="ConsoleMiddleware"/>,
/// and the socket path is read by <see cref="WorkerNotificationService"/>.
/// </summary>
/// <remarks>
/// The port is exposed as an awaitable <see cref="Ready"/> task so that
/// consumers started before the worker host's <c>ExecuteAsync</c> can
/// block until the port is known. The socket path is exposed via the
/// awaitable <see cref="SocketPathReady"/> task for the same reason —
/// <see cref="WorkerProcessHost"/> and <see cref="WorkerNotificationService"/>
/// run as concurrent background services and resolve their
/// <see cref="WorkerOptions"/> from different DI option caches
/// (<c>IOptions</c> vs <c>IOptionsMonitor</c> hand out distinct instances),
/// so the notifier must read the path the host actually launched the worker
/// with rather than independently re-deriving an auto-path. Tests inject a
/// stub endpoint with the port pre-set.
/// </remarks>
public sealed class InternalWorkerEndpoint
{
    private readonly TaskCompletionSource<int> _readyTcs =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource<string?> _socketPathTcs =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private volatile int _port;
    private int _socketPathSet;

    /// <summary>
    /// Loopback port the worker is listening on, or 0 if not set yet.
    /// </summary>
    public int Port => _port;

    /// <summary>
    /// Task that completes once <see cref="SetPort(int)"/> has been
    /// called. Result is the port.
    /// </summary>
    public Task<int> Ready => _readyTcs.Task;

    /// <summary>
    /// Task that completes once <see cref="SetSocketPath(string?)"/> has been
    /// called. Result is the worker's IPC endpoint path, or <c>null</c> when
    /// worker coordination is disabled.
    /// </summary>
    public Task<string?> SocketPathReady => _socketPathTcs.Task;

    /// <summary>
    /// Sets the loopback port. Idempotent: a second call with the same
    /// port is a no-op; a second call with a different port throws.
    /// </summary>
    public void SetPort(int port)
    {
        if (port <= 0 || port > 65535)
            throw new ArgumentOutOfRangeException(nameof(port), port,
                "port must be in [1, 65535]");

        var existing = Interlocked.CompareExchange(ref _port, port, 0);
        if (existing == 0)
        {
            _readyTcs.TrySetResult(port);
        }
        else if (existing != port)
        {
            throw new InvalidOperationException(
                $"InternalWorkerEndpoint already set to {existing}; cannot reassign to {port}");
        }
    }

    /// <summary>
    /// Publishes the worker's IPC endpoint path (or <c>null</c> when worker
    /// coordination is disabled). Idempotent on the first call; subsequent
    /// calls are ignored so an accidental double-publish can't reassign it.
    /// </summary>
    public void SetSocketPath(string? socketPath)
    {
        if (Interlocked.CompareExchange(ref _socketPathSet, 1, 0) == 0)
            _socketPathTcs.TrySetResult(socketPath);
    }
}

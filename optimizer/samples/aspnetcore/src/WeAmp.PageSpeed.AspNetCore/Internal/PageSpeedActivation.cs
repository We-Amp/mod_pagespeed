// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The one point where PageSpeed becomes active: the first time
/// <see cref="PageSpeedOptions.Enabled"/> is true, at startup or through a
/// later hot reload.
/// </summary>
/// <remarks>
/// <para>Activation opens the real cache (when this instance was given a way
/// to open one) and then completes <see cref="Activated"/>, which
/// <see cref="WorkerProcessHost"/> waits on before it launches the worker and
/// publishes its socket path and port. Until then nothing touches the disk:
/// no cache volume, no cache directory, no worker.</para>
///
/// <para>Activation happens once. <c>Enabled</c> going back to false tears
/// nothing down; the middleware passes responses through, and the open cache
/// and the running worker stay as they are for the next switch-on.</para>
///
/// <para>Two paths reach it. A configuration reload that sets <c>Enabled</c>
/// runs it from the options change notification, and the cache wrapper runs
/// it from the request path when it sees <c>Enabled</c> before that
/// notification has arrived: <see cref="IOptionsMonitor{TOptions}"/> updates
/// <c>CurrentValue</c> before it calls its listeners.</para>
///
/// <para>When <c>Enabled</c> is true at startup, a cache that cannot be opened
/// throws, as it always has, so the application fails to start. When the cache
/// cannot be opened on a later switch-on, the failure is logged and activation
/// is retried at the next configuration change.</para>
/// </remarks>
internal sealed class PageSpeedActivation : IDisposable
{
    private readonly IOptionsMonitor<PageSpeedOptions> _monitor;
    private readonly Func<PageSpeedOptions, IPageSpeedCache>? _openCache;
    private readonly ILogger _logger;
    private readonly object _lock = new();
    private readonly TaskCompletionSource<bool> _activated =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private IDisposable? _registration;
    private volatile IPageSpeedCache? _cache;
    // Set when opening the cache failed; the request path then leaves the
    // retry to the next configuration change instead of retrying per request.
    private volatile bool _openFailed;
    // The last failure to open the cache, cleared when opening succeeds.
    private volatile Exception? _openFailure;
    // 1 once the "cache is not open" write skip was reported for the current
    // failure state; reset by every new failure.
    private int _notOpenReported;
    // Set before _cache is cleared, so a reader that finds no cache after
    // dispose also sees this.
    private volatile bool _disposed;

    /// <param name="monitor">The options whose <c>Enabled</c> switch is followed.</param>
    /// <param name="openCache">
    /// Opens the real cache on activation; <c>null</c> for an activation that
    /// only signals (a worker host built on its own).
    /// </param>
    /// <param name="logger">Optional logger.</param>
    public PageSpeedActivation(
        IOptionsMonitor<PageSpeedOptions> monitor,
        Func<PageSpeedOptions, IPageSpeedCache>? openCache,
        ILogger? logger = null)
    {
        _monitor = monitor;
        _openCache = openCache;
        _logger = logger ?? NullLogger.Instance;
        Cache = new DeferredPageSpeedCache(this);

        // Subscribe before reading the current value, so a reload that lands
        // between the two is not missed.
        _registration = monitor.OnChange(_ =>
        {
            _openFailed = false;
            TryActivate();
        });

        if (monitor.CurrentValue.Enabled)
        {
            try
            {
                Activate(throwOnFailure: true);
            }
            catch
            {
                _registration?.Dispose();
                _registration = null;
                throw;
            }
        }
        else if (_openCache != null)
        {
            _logger.LogInformation(
                "PageSpeed is disabled; the cache volume opens when "
                + "PageSpeed:Enabled becomes true, no restart needed");
        }
    }

    /// <summary>
    /// The cache the middleware uses: empty until activation, then the real
    /// cache.
    /// </summary>
    public DeferredPageSpeedCache Cache { get; }

    /// <summary>Completes once, when PageSpeed is first enabled.</summary>
    public Task Activated => _activated.Task;

    /// <summary>The real cache, or <c>null</c> before activation.</summary>
    internal IPageSpeedCache? ActiveCache => _cache;

    /// <summary>
    /// Why the cache could not be opened when PageSpeed was enabled, or
    /// <c>null</c> when no attempt failed (or the cache is open).
    /// </summary>
    internal Exception? OpenFailure => _cache == null ? _openFailure : null;

    /// <summary>
    /// True once per failure state: the first caller that skips a cache
    /// write because the cache is not open reports it, the rest stay quiet
    /// until opening fails again.
    /// </summary>
    internal bool TryClaimNotOpenReport() =>
        Interlocked.Exchange(ref _notOpenReported, 1) == 0;

    /// <summary>
    /// Activates when <c>Enabled</c> is true now and activation has not
    /// happened yet. Returns the real cache, or <c>null</c> while inactive.
    /// Cheap once active, and cheap while disabled.
    /// </summary>
    /// <exception cref="ObjectDisposedException">
    /// This activation was disposed: the cache is closed for good, whether it
    /// was ever opened or not, and is never opened again.
    /// </exception>
    internal IPageSpeedCache? TryActivateFromRequest()
    {
        var cache = _cache;
        if (cache == null && !_openFailed) cache = TryActivate();
        if (cache == null && _disposed)
            throw new ObjectDisposedException(nameof(IPageSpeedCache));
        return cache;
    }

    private IPageSpeedCache? TryActivate()
    {
        if (_disposed || _activated.Task.IsCompleted) return _cache;
        if (!_monitor.CurrentValue.Enabled) return null;
        return Activate(throwOnFailure: false);
    }

    private IPageSpeedCache? Activate(bool throwOnFailure)
    {
        lock (_lock)
        {
            if (_disposed || _activated.Task.IsCompleted) return _cache;

            if (_openCache != null)
            {
                try
                {
                    _cache = _openCache(_monitor.CurrentValue);
                }
                catch (Exception ex) when (!throwOnFailure)
                {
                    _openFailed = true;
                    // One reload can raise several change notifications, and
                    // each retries. Log a failure once while it stays the same
                    // failure; a different one is a new failure state.
                    var previous = _openFailure;
                    _openFailure = ex;
                    if (previous != null
                        && previous.GetType() == ex.GetType()
                        && previous.Message == ex.Message)
                    {
                        _logger.LogDebug(ex,
                            "Opening the PageSpeed cache volume failed again");
                        return null;
                    }
                    Interlocked.Exchange(ref _notOpenReported, 0);
                    _logger.LogError(ex,
                        "PageSpeed was enabled, but opening the cache volume failed; "
                        + "PageSpeed stays inactive and retries at the next "
                        + "configuration change");
                    return null;
                }
                _logger.LogInformation("PageSpeed is enabled; opened the cache volume");
            }

            // Activation is one-way: stop listening once it happened.
            _registration?.Dispose();
            _registration = null;
            _activated.TrySetResult(true);
            return _cache;
        }
    }

    /// <summary>Stops listening and disposes the real cache, if open.</summary>
    public void Dispose()
    {
        IPageSpeedCache? cache;
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            _registration?.Dispose();
            _registration = null;
            cache = _cache;
            _cache = null;
        }
        cache?.Dispose();
    }
}

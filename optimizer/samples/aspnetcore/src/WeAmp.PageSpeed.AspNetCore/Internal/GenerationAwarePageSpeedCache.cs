// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The cache the middleware uses: a <see cref="IPageSpeedCache"/> that
/// follows the worker's volume resets.
/// </summary>
/// <remarks>
/// <para>A full purge (<c>POST /v1/cache/purge</c> with
/// <c>scope: "all"</c>) makes the worker delete the volume file and create a
/// fresh one, then bump the counter in <c>{VolumePath}.gen</c>
/// (PageSpeedCache::ResetVolume). A process that keeps its open cache keeps
/// mapping the deleted file and serves its pre-purge entries as HITs until it
/// restarts. The nginx module follows the counter
/// (CheckGenerationAndGetCache); this class is the middleware's equivalent.</para>
///
/// <para>At most once per <c>checkInterval</c> (1 s, the nginx module's
/// cadence) one request reads the counter file; every other request pays one
/// clock read and a compare. When the counter moved, a fresh cache is opened
/// and published, and the retired instance is disposed once no call is still
/// running on it. Read results and writers already handed out stay valid
/// after that: each holds its own reference on the native cache (see
/// <see cref="SafeCacheChildHandle"/>), so a response still streaming from
/// the old volume completes.</para>
///
/// <para>If the fresh cache cannot be opened, the current one stays in use
/// and the reopen is retried at the next check. Until a retry succeeds the
/// middleware keeps serving what the previous volume holds, pre-purge entries
/// included; the nginx module serves uncached in that case instead. The
/// failure is logged once per counter value.</para>
///
/// <para>Closing a native cache takes 100-200 ms, so a retired instance is
/// disposed on the thread pool, never on the request thread that happened to
/// release it last and never under the swap lock.</para>
/// </remarks>
internal sealed class GenerationAwarePageSpeedCache : IPageSpeedCache
{
    internal static readonly TimeSpan DefaultCheckInterval = TimeSpan.FromSeconds(1);

    /// <summary>One opened cache plus the generation it was opened at.</summary>
    private sealed class Slot
    {
        public readonly IPageSpeedCache Cache;
        public readonly ulong Generation;
        // One reference is owned by the decorator while this slot is current;
        // each in-flight call holds one more. Zero = disposed, never revived.
        private int _refs = 1;
        // Set by Retire before the owner reference is dropped; the
        // Interlocked.Decrement in Release orders the write before any
        // thread that observes zero.
        private Action<IPageSpeedCache>? _disposer;

        public Slot(IPageSpeedCache cache, ulong generation)
        {
            Cache = cache;
            Generation = generation;
        }

        public bool TryAcquire()
        {
            while (true)
            {
                int refs = Volatile.Read(ref _refs);
                if (refs == 0) return false;
                if (Interlocked.CompareExchange(ref _refs, refs + 1, refs) == refs)
                    return true;
            }
        }

        /// <summary>Drops the owner reference; the last release disposes via
        /// <paramref name="disposer"/>.</summary>
        public void Retire(Action<IPageSpeedCache> disposer)
        {
            Volatile.Write(ref _disposer, disposer);
            Release();
        }

        public void Release()
        {
            if (Interlocked.Decrement(ref _refs) != 0) return;
            var disposer = Volatile.Read(ref _disposer);
            if (disposer != null) disposer(Cache);
            else Cache.Dispose();
        }
    }

    /// <summary>What <see cref="ReadGeneration"/> found.</summary>
    internal enum GenerationReadStatus
    {
        /// <summary>The file was read (or is missing: generation 0).</summary>
        Ok,
        /// <summary>The file exists but could not be read; try again later.</summary>
        Unreadable,
    }

    private readonly Func<IPageSpeedCache> _factory;
    private readonly string _generationPath;
    private readonly long _intervalMs;
    private readonly Func<long> _clockMs;
    private readonly Action<Action> _disposeScheduler;
    private readonly ILogger _logger;
    private readonly object _swapLock = new();
    private Slot? _current; // null once disposed
    private long _nextCheckMs;
    // Log-once state, touched only under _swapLock or by the single caller
    // that won the check slot; benign if two checks race on it.
    private ulong? _reopenFailureLoggedFor;
    private bool _unreadableLogged;

    /// <param name="factory">Opens a cache on the configured volume.</param>
    /// <param name="generationPath">
    /// The worker's generation counter file, <c>{VolumePath}.gen</c>.
    /// </param>
    /// <param name="logger">Optional logger.</param>
    /// <param name="checkInterval">
    /// Minimum time between two reads of the counter file.
    /// </param>
    /// <param name="clockMs">Monotonic milliseconds; tests substitute it.</param>
    /// <param name="disposeScheduler">
    /// Runs the disposal of a retired cache. Defaults to the thread pool;
    /// tests pass an inline runner.
    /// </param>
    public GenerationAwarePageSpeedCache(
        Func<IPageSpeedCache> factory,
        string generationPath,
        ILogger? logger = null,
        TimeSpan? checkInterval = null,
        Func<long>? clockMs = null,
        Action<Action>? disposeScheduler = null)
    {
        _factory = factory;
        _generationPath = generationPath;
        _logger = logger ?? NullLogger.Instance;
        _intervalMs = (long)(checkInterval ?? DefaultCheckInterval).TotalMilliseconds;
        _clockMs = clockMs ?? (() => Environment.TickCount64);
        _disposeScheduler = disposeScheduler
            ?? (work => ThreadPool.UnsafeQueueUserWorkItem(static w => w(), work, preferLocal: false));

        // Read the counter BEFORE opening: a reset that lands between the two
        // then shows up as a changed counter at the next check (one extra
        // reopen at worst) instead of being recorded as already seen.
        ReadGeneration(_generationPath, out ulong generation);
        _current = new Slot(_factory(), generation);
        _nextCheckMs = _clockMs() + _intervalMs;
    }

    /// <summary>
    /// Reads the counter the way PageSpeedCache::ReadGenerationFile does:
    /// a missing or unparsable file is generation 0.
    /// </summary>
    /// <remarks>
    /// The file is opened with read, write and delete sharing: the worker
    /// replaces it with a rename (MoveFileEx with MOVEFILE_REPLACE_EXISTING on
    /// Windows), which a reader holding it without delete sharing would make
    /// fail.
    /// </remarks>
    internal static GenerationReadStatus ReadGeneration(string path, out ulong generation)
    {
        generation = 0;
        string text;
        try
        {
            using var stream = new FileStream(
                path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete,
                bufferSize: 1, FileOptions.None);
            using var reader = new StreamReader(stream);
            text = reader.ReadToEnd();
        }
        catch (FileNotFoundException)
        {
            return GenerationReadStatus.Ok;
        }
        catch (DirectoryNotFoundException)
        {
            return GenerationReadStatus.Ok;
        }
        catch (IOException)
        {
            return GenerationReadStatus.Unreadable;
        }
        catch (UnauthorizedAccessException)
        {
            return GenerationReadStatus.Unreadable;
        }
        if (ulong.TryParse(text.AsSpan().Trim(), out var parsed)) generation = parsed;
        return GenerationReadStatus.Ok;
    }

    /// <summary>The generation of the cache currently in use (for tests).</summary>
    internal ulong CurrentGeneration =>
        (Volatile.Read(ref _current) ?? throw new ObjectDisposedException(GetType().Name))
            .Generation;

    private void MaybeCheckGeneration()
    {
        long now = _clockMs();
        long next = Volatile.Read(ref _nextCheckMs);
        if (now < next) return;
        // One caller per interval wins the slot and does the file read.
        if (Interlocked.CompareExchange(ref _nextCheckMs, now + _intervalMs, next) != next)
            return;
        CheckGenerationNow();
    }

    private bool TryReadGeneration(out ulong generation)
    {
        if (ReadGeneration(_generationPath, out generation) == GenerationReadStatus.Ok)
        {
            _unreadableLogged = false;
            return true;
        }
        if (!_unreadableLogged)
        {
            _unreadableLogged = true;
            _logger.LogWarning(
                "PageSpeed cache generation file {Path} exists but cannot be read; "
                + "a full cache purge is not noticed until it can",
                _generationPath);
        }
        return false;
    }

    /// <summary>
    /// Reads the counter file and, when it moved, swaps to a freshly opened
    /// cache. Not throttled.
    /// </summary>
    internal void CheckGenerationNow()
    {
        if (!TryReadGeneration(out ulong generation)) return;
        var current = Volatile.Read(ref _current);
        if (current == null || current.Generation == generation) return;

        Slot retired;
        lock (_swapLock)
        {
            // Re-read under the lock: a caller that waited here behind a swap
            // acts on the newest counter, not on the one it read before.
            if (!TryReadGeneration(out generation)) return;
            current = Volatile.Read(ref _current);
            if (current == null || current.Generation == generation) return;

            IPageSpeedCache fresh;
            try
            {
                fresh = _factory();
            }
            catch (Exception ex)
            {
                if (_reopenFailureLoggedFor != generation)
                {
                    _reopenFailureLoggedFor = generation;
                    _logger.LogWarning(ex,
                        "PageSpeed cache generation changed {Old} -> {New} (volume reset), "
                        + "but reopening the cache failed; still serving the previous "
                        + "volume, retrying every check",
                        current.Generation, generation);
                }
                return;
            }

            // Publish first, then drop the owner reference (below, outside the
            // lock): a caller that read the old slot and fails to acquire it
            // re-reads and finds this one.
            Volatile.Write(ref _current, new Slot(fresh, generation));
            _reopenFailureLoggedFor = null;
            retired = current;
        }
        retired.Retire(DisposeRetired);
        _logger.LogInformation(
            "PageSpeed cache generation changed {Old} -> {New} (volume reset), "
            + "reopened the cache",
            retired.Generation, generation);
    }

    private void DisposeRetired(IPageSpeedCache cache)
    {
        _disposeScheduler(() =>
        {
            try
            {
                cache.Dispose();
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Disposing a retired PageSpeed cache failed");
            }
        });
    }

    private Slot Acquire()
    {
        MaybeCheckGeneration();
        while (true)
        {
            var slot = Volatile.Read(ref _current)
                ?? throw new ObjectDisposedException(GetType().Name);
            if (slot.TryAcquire()) return slot;
            // Retired between the read and the acquire; its successor is
            // already published.
        }
    }

    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadBest(url, hostname, scheme, mask); }
        finally { slot.Release(); }
    }

    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadBestAgent(url, hostname, scheme, mask, agentEntitled); }
        finally { slot.Release(); }
    }

    public int ReadSharedConfigAgentEntitled()
    {
        var slot = Acquire();
        try { return slot.Cache.ReadSharedConfigAgentEntitled(); }
        finally { slot.Release(); }
    }

    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadAlternate(url, hostname, scheme, alternateId); }
        finally { slot.Release(); }
    }

    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
    {
        var slot = Acquire();
        try { return slot.Cache.ReadEarlyHints(url, hostname, scheme); }
        finally { slot.Release(); }
    }

    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams p)
    {
        var slot = Acquire();
        try { return slot.Cache.BeginWrite(url, hostname, scheme, p); }
        finally { slot.Release(); }
    }

    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId, ulong contentLength)
    {
        var slot = Acquire();
        try
        {
            return slot.Cache.BeginWriteSentinel(
                url, hostname, scheme, sentinelId, contentLength);
        }
        finally { slot.Release(); }
    }

    public HeadersSidecarWriteResult WriteHeadersSidecar(
        string url, string hostname, string scheme,
        IReadOnlyList<KeyValuePair<string, string>> headers)
    {
        var slot = Acquire();
        try { return slot.Cache.WriteHeadersSidecar(url, hostname, scheme, headers); }
        finally { slot.Release(); }
    }

    public CacheStats GetStats()
    {
        var slot = Acquire();
        try { return slot.Cache.GetStats(); }
        finally { slot.Release(); }
    }

    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
    {
        var slot = Acquire();
        try { slot.Cache.RecordServeHit(type, originalBytes, optimizedBytes, mask); }
        finally { slot.Release(); }
    }

    /// <summary>
    /// The current instance's handle, for a native call made by the caller.
    /// A swap can close it between this getter and that call; the call then
    /// fails with <see cref="ObjectDisposedException"/> and the caller reads
    /// this property again (see HtmlProcessor).
    /// </summary>
    SafeCacheHandle IPageSpeedCache.Handle
    {
        get
        {
            var slot = Volatile.Read(ref _current)
                ?? throw new ObjectDisposedException(GetType().Name);
            return slot.Cache.Handle;
        }
    }

    public void Dispose()
    {
        lock (_swapLock)
        {
            Interlocked.Exchange(ref _current, null)?.Release();
        }
    }
}

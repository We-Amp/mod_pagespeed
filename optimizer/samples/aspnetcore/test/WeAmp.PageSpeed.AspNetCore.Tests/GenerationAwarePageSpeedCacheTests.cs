// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Logging;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.Native;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The swap logic that makes the middleware follow a worker volume reset
/// (full purge): the counter in <c>{VolumePath}.gen</c> moves, and the
/// middleware must reopen its cache instead of serving the deleted volume
/// until restart. Native-free: the caches are fakes.
/// </summary>
public sealed class GenerationAwarePageSpeedCacheTests : IDisposable
{
    private readonly string _dir;
    private readonly string _genPath;
    private long _nowMs = 1_000_000;
    private readonly List<FakeCache> _opened = new();
    private int _factoryCalls;
    // Called with the 1-based factory call number.
    private Func<int, FakeCache>? _factoryOverride;
    private readonly ListLogger _log = new();
    // Retired caches are disposed through this; null = inline.
    private Action<Action>? _disposeScheduler;

    public GenerationAwarePageSpeedCacheTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), $"ps_gen_{Guid.NewGuid():N}");
        Directory.CreateDirectory(_dir);
        _genPath = Path.Combine(_dir, "volume.dat.gen");
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { }
    }

    private GenerationAwarePageSpeedCache Create(TimeSpan? interval = null) =>
        new(
            () =>
            {
                lock (_opened)
                {
                    int id = ++_factoryCalls;
                    var cache = _factoryOverride?.Invoke(id) ?? new FakeCache(id);
                    _opened.Add(cache);
                    return cache;
                }
            },
            _genPath,
            logger: _log,
            checkInterval: interval ?? TimeSpan.FromSeconds(1),
            clockMs: () => Interlocked.Read(ref _nowMs),
            disposeScheduler: _disposeScheduler ?? (work => work()));

    // Written the way the worker writes it: "%llu\n", replaced atomically.
    private void WriteGeneration(ulong gen)
    {
        var tmp = _genPath + ".tmp";
        File.WriteAllText(tmp, $"{gen}\n");
        File.Move(tmp, _genPath, overwrite: true);
    }

    private void Advance(long ms) => Interlocked.Add(ref _nowMs, ms);

    // Which fake served the call (the fakes answer with their id).
    private static int Served(GenerationAwarePageSpeedCache c) =>
        c.ReadSharedConfigAgentEntitled();

    private ulong? Read()
    {
        var status = GenerationAwarePageSpeedCache.ReadGeneration(_genPath, out var gen);
        return status == GenerationAwarePageSpeedCache.GenerationReadStatus.Ok ? gen : null;
    }

    [Fact]
    public void ReadGeneration_ParsesWorkerFormat_MissingIsZero()
    {
        Assert.Equal(0UL, Read());
        WriteGeneration(42);
        Assert.Equal(42UL, Read());
        File.WriteAllText(_genPath, "garbage");
        Assert.Equal(0UL, Read());
    }

    [Fact]
    public void ReadGeneration_DoesNotBlockTheWorkersReplace()
    {
        // The worker replaces the file by rename; a reader must hold it with
        // delete sharing so that rename can succeed (matters on Windows).
        WriteGeneration(1);
        using (var held = new FileStream(_genPath, FileMode.Open, FileAccess.Read,
                   FileShare.ReadWrite | FileShare.Delete))
        {
            WriteGeneration(2);
        }
        Assert.Equal(2UL, Read());
    }

    [Fact]
    public void UnreadableGenerationFile_SkipsTheCheck_AndLogsOnce()
    {
        WriteGeneration(1);
        using var cache = Create();
        // A directory where the file should be cannot be read as one.
        File.Delete(_genPath);
        Directory.CreateDirectory(_genPath);
        Assert.Null(Read());
        for (int i = 0; i < 3; i++)
        {
            Advance(1000);
            Assert.Equal(1, Served(cache));
        }
        Assert.Single(_opened);
        Assert.Equal(1, _log.Count(LogLevel.Warning, "cannot be read"));
    }

    [Fact]
    public void UnchangedGeneration_KeepsTheSameCache()
    {
        WriteGeneration(3);
        using var cache = Create();
        Assert.Equal(3UL, cache.CurrentGeneration);
        for (int i = 0; i < 10; i++)
        {
            Advance(1500);
            Assert.Equal(1, Served(cache));
        }
        Assert.Single(_opened);
        Assert.Equal(0, _opened[0].DisposeCount);
    }

    [Fact]
    public void GenerationChange_ReopensAfterTheInterval_AndDisposesTheOldCache()
    {
        WriteGeneration(1);
        using var cache = Create();
        Assert.Equal(1, Served(cache));

        // Worker resets the volume.
        WriteGeneration(2);

        // Throttled: no file read before the interval has elapsed.
        Advance(500);
        Assert.Equal(1, Served(cache));
        Assert.Single(_opened);

        Advance(600);
        Assert.Equal(2, Served(cache));
        Assert.Equal(2UL, cache.CurrentGeneration);
        Assert.Equal(2, _opened.Count);
        Assert.Equal(1, _opened[0].DisposeCount);
        Assert.Equal(0, _opened[1].DisposeCount);

        // A second reset is followed too.
        WriteGeneration(3);
        Advance(1000);
        Assert.Equal(3, Served(cache));
        Assert.Equal(1, _opened[1].DisposeCount);
    }

    [Fact]
    public void GenerationGoingBackToZero_AlsoReopens()
    {
        // A deleted counter file reads as 0 (as in the native reader); any
        // difference from the opened generation is a change.
        WriteGeneration(5);
        using var cache = Create();
        File.Delete(_genPath);
        Advance(1000);
        Assert.Equal(2, Served(cache));
        Assert.Equal(0UL, cache.CurrentGeneration);
    }

    [Fact]
    public void RetiredCache_IsNotDisposedWhileACallIsStillRunningOnIt()
    {
        WriteGeneration(1);
        GenerationAwarePageSpeedCache? cache = null;
        int innerServed = 0;
        bool disposedDuringCall = true;

        _factoryOverride = id => new FakeCache(id)
        {
            DuringCall = self =>
            {
                if (self.Id != 1 || innerServed != 0) return;
                // While a call is running on cache 1, a reset lands and
                // another request swaps to cache 2.
                WriteGeneration(2);
                Advance(1000);
                innerServed = Served(cache!);
                disposedDuringCall = self.DisposeCount != 0;
            },
        };
        cache = Create();
        using var _ = cache;

        Assert.Equal(1, Served(cache));
        Assert.Equal(2, innerServed);
        Assert.False(disposedDuringCall,
            "the retired cache was disposed while a call was still running on it");
        Assert.Equal(1, _opened[0].DisposeCount);
    }

    [Fact]
    public void FailedReopen_KeepsTheCurrentCache_AndRetriesAtTheNextCheck()
    {
        WriteGeneration(1);
        _factoryOverride = id => id is 2 or 3
            ? throw new PageSpeedException(PageSpeedError.IoError, "volume busy")
            : new FakeCache(id);
        using var cache = Create();

        WriteGeneration(2);
        Advance(1000);
        Assert.Equal(1, Served(cache));
        Assert.Equal(1UL, cache.CurrentGeneration);
        Assert.Equal(0, _opened[0].DisposeCount);

        Advance(1000);
        Assert.Equal(1, Served(cache)); // factory call #3 fails too
        Assert.Equal(1, _log.Count(LogLevel.Warning, "reopening the cache failed"));

        Advance(1000);
        Assert.Equal(4, Served(cache)); // factory call #4 succeeds
        Assert.Equal(2UL, cache.CurrentGeneration);
        Assert.Equal(1, _opened[0].DisposeCount);
    }

    [Fact]
    public void RetiredCache_IsDisposedThroughTheScheduler_NotInline()
    {
        var queued = new List<Action>();
        _disposeScheduler = work => queued.Add(work);
        WriteGeneration(1);
        using var cache = Create();

        WriteGeneration(2);
        Advance(1000);
        Assert.Equal(2, Served(cache));
        Assert.Equal(0, _opened[0].DisposeCount);
        Assert.Single(queued);

        queued[0]();
        Assert.Equal(1, _opened[0].DisposeCount);
    }

    [Fact]
    public void Dispose_DisposesTheCurrentCache_AndRefusesFurtherCalls()
    {
        var cache = Create();
        cache.Dispose();
        cache.Dispose();
        Assert.Equal(1, _opened[0].DisposeCount);
        Assert.Throws<ObjectDisposedException>(() => Served(cache));
    }

    [Fact]
    public async Task ConcurrentCallsAcrossManySwaps_NeverUseADisposedCache()
    {
        WriteGeneration(1);
        // Interval 0: every call may check, maximising swap/acquire races.
        using var cache = Create(TimeSpan.Zero);
        using var stop = new CancellationTokenSource();
        var errors = new List<Exception>();

        var readers = Enumerable.Range(0, 8).Select(_ => Task.Run(() =>
        {
            while (!stop.IsCancellationRequested)
            {
                try
                {
                    // FakeCache throws ObjectDisposedException when called
                    // after its Dispose.
                    Served(cache);
                    cache.GetStats();
                }
                catch (Exception ex)
                {
                    lock (errors) errors.Add(ex);
                    return;
                }
            }
        })).ToArray();

        for (ulong gen = 2; gen < 200; gen++)
        {
            WriteGeneration(gen);
            cache.CheckGenerationNow();
            await Task.Yield();
        }
        stop.Cancel();
        await Task.WhenAll(readers);

        Assert.Empty(errors);
        lock (_opened)
        {
            Assert.True(_opened.Count >= 2);
            // Every retired cache disposed exactly once; the current one not.
            for (int i = 0; i < _opened.Count - 1; i++)
                Assert.Equal(1, _opened[i].DisposeCount);
            Assert.Equal(0, _opened[^1].DisposeCount);
        }
    }

    private sealed class ListLogger : ILogger
    {
        private readonly List<(LogLevel Level, string Message)> _entries = new();

        public int Count(LogLevel level, string contains)
        {
            lock (_entries)
                return _entries.Count(e => e.Level == level && e.Message.Contains(contains));
        }

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            lock (_entries) _entries.Add((logLevel, formatter(state, exception)));
        }
    }

    /// <summary>
    /// Native-free cache. NSubstitute cannot proxy IPageSpeedCache (internal
    /// Handle member), so this is hand-written like the other test fakes.
    /// </summary>
    private sealed class FakeCache : IPageSpeedCache
    {
        private int _disposeCount;

        public FakeCache(int id) => Id = id;

        public int Id { get; }
        public int DisposeCount => Volatile.Read(ref _disposeCount);
        public Action<FakeCache>? DuringCall { get; init; }

        private void Enter()
        {
            if (DisposeCount != 0)
                throw new ObjectDisposedException($"FakeCache {Id}");
            DuringCall?.Invoke(this);
        }

        SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException("Test fake has no native handle");

        public int ReadSharedConfigAgentEntitled()
        {
            Enter();
            return Id;
        }

        public CacheStats GetStats()
        {
            Enter();
            return default!;
        }

        public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
        {
            Enter();
            return null;
        }

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled)
        {
            Enter();
            return null;
        }

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId)
        {
            Enter();
            return null;
        }

        public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
        {
            Enter();
            return null;
        }

        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException();

        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId,
            ulong contentLength) =>
            throw new NotSupportedException();

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) =>
            Enter();

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.Native;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// The cache <c>AddPageSpeed</c> registers is disposed with the service
/// provider. From then on every use throws
/// <see cref="ObjectDisposedException"/>, whether PageSpeed was active or not,
/// the real cache is closed exactly once, and nothing opens a new one.
/// </summary>
public sealed class DeferredPageSpeedCacheDisposeTests : IDisposable
{
    private readonly string _baseDir =
        Path.Combine(Path.GetTempPath(), $"ps_dispose_{Guid.NewGuid():N}");

    public void Dispose()
    {
        try { Directory.Delete(_baseDir, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    [Fact]
    public void Active_Dispose_ClosesTheRealCacheOnceAndRejectsFurtherUse()
    {
        var monitor = new MutableOptionsMonitor(enabled: true);
        var opened = new List<CountingCache>();
        var activation = new PageSpeedActivation(monitor, _ =>
        {
            var c = new CountingCache();
            opened.Add(c);
            return c;
        });
        var cache = activation.Cache;
        Assert.Single(opened);
        cache.GetStats();

        // The service provider disposes both singletons.
        cache.Dispose();
        activation.Dispose();
        cache.Dispose();

        Assert.Equal(1, opened[0].DisposeCount);
        AssertRejectsUse(cache);

        // A later switch-on must not reopen a cache after dispose.
        monitor.Set(enabled: false);
        monitor.Set(enabled: true);
        AssertRejectsUse(cache);
        Assert.Single(opened);
        Assert.Equal(1, opened[0].DisposeCount);
    }

    [Fact]
    public void Inactive_Dispose_RejectsFurtherUseAndNeverOpens()
    {
        var monitor = new MutableOptionsMonitor(enabled: false);
        var opens = 0;
        var activation = new PageSpeedActivation(monitor, _ =>
        {
            opens++;
            return new CountingCache();
        });
        var cache = activation.Cache;
        Assert.Null(cache.ReadBest("/", "example.test", "https", 0));

        cache.Dispose();
        activation.Dispose();
        AssertRejectsUse(cache);

        // Enabled after dispose: the request path must not open a cache.
        monitor.Set(enabled: true);
        AssertRejectsUse(cache);
        Assert.Equal(0, opens);
        Assert.Null(activation.ActiveCache);
    }

    [Fact]
    public async Task DisposeRacingFirstUse_NeverLeavesAnOpenCache()
    {
        for (var i = 0; i < 200; i++)
        {
            var monitor = new MutableOptionsMonitor(enabled: false);
            var opened = new System.Collections.Concurrent.ConcurrentBag<CountingCache>();
            var activation = new PageSpeedActivation(monitor, _ =>
            {
                var c = new CountingCache();
                opened.Add(c);
                return c;
            });
            var cache = activation.Cache;
            monitor.Value.Enabled = true; // seen by the request path only

            using var start = new ManualResetEventSlim();
            var use = Task.Run(() =>
            {
                start.Wait();
                try { cache.GetStats(); }
                catch (ObjectDisposedException) { }
            });
            var dispose = Task.Run(() =>
            {
                start.Wait();
                cache.Dispose();
            });
            start.Set();
            await Task.WhenAll(use, dispose);

            Assert.True(opened.Count <= 1);
            Assert.All(opened, c => Assert.Equal(1, c.DisposeCount));
            AssertRejectsUse(cache);
        }
    }

    [NativeLibFact]
    public async Task AddPageSpeed_CacheRejectsUseAfterDisposeAsync()
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.WebHost.UseTestServer();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddPageSpeed(opts =>
        {
            opts.Cache.VolumePath = Path.Combine(_baseDir, "volume.dat");
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = false;
        });
        var app = builder.Build();
        app.UsePageSpeed();
        await app.StartAsync();

        var cache = app.Services.GetRequiredService<IPageSpeedCache>();
        await app.StopAsync();
        cache.GetStats(); // still open after StopAsync

        await app.DisposeAsync();
        Assert.Throws<ObjectDisposedException>(() => cache.GetStats());
        Assert.Throws<ObjectDisposedException>(
            () => cache.ReadBest("/", "localhost", "https", 0));
    }

    private static void AssertRejectsUse(DeferredPageSpeedCache cache)
    {
        IPageSpeedCache c = cache;
        Assert.Throws<ObjectDisposedException>(() => c.GetStats());
        Assert.Throws<ObjectDisposedException>(() => c.ReadBest("/", "example.test", "https", 0));
        Assert.Throws<ObjectDisposedException>(
            () => c.ReadEarlyHints("/", "example.test", "https"));
        Assert.Throws<ObjectDisposedException>(() => c.ReadSharedConfigAgentEntitled());
        Assert.Throws<ObjectDisposedException>(
            () => c.RecordServeHit(default, 1, 1, 0));
        Assert.Throws<ObjectDisposedException>(() => c.Handle);
        Assert.Throws<ObjectDisposedException>(() => cache.TryGetHandle());
    }

    private sealed class MutableOptionsMonitor(bool enabled)
        : IOptionsMonitor<PageSpeedOptions>
    {
        private readonly List<Action<PageSpeedOptions, string?>> _listeners = new();

        public PageSpeedOptions Value { get; private set; } =
            new() { Enabled = enabled };

        public PageSpeedOptions CurrentValue => Value;

        public PageSpeedOptions Get(string? name) => Value;

        public void Set(bool enabled)
        {
            Value = new PageSpeedOptions { Enabled = enabled };
            List<Action<PageSpeedOptions, string?>> listeners;
            lock (_listeners) listeners = _listeners.ToList();
            foreach (var l in listeners) l(Value, null);
        }

        public IDisposable? OnChange(Action<PageSpeedOptions, string?> listener)
        {
            lock (_listeners) _listeners.Add(listener);
            return new Unsubscribe(() => { lock (_listeners) _listeners.Remove(listener); });
        }

        private sealed class Unsubscribe(Action action) : IDisposable
        {
            public void Dispose() => action();
        }
    }

    private sealed class CountingCache : IPageSpeedCache
    {
        private int _disposeCount;

        public int DisposeCount => Volatile.Read(ref _disposeCount);

        private void Enter()
        {
            if (DisposeCount != 0) throw new ObjectDisposedException(nameof(CountingCache));
        }

        SafeCacheHandle IPageSpeedCache.Handle
        {
            get
            {
                Enter();
                throw new NotSupportedException("Test fake has no native handle");
            }
        }

        public int ReadSharedConfigAgentEntitled()
        {
            Enter();
            return 0;
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

        public HeadersSidecarWriteResult WriteHeadersSidecar(
            string url, string hostname, string scheme,
            IReadOnlyList<KeyValuePair<string, string>> headers) =>
            throw new NotSupportedException();

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) =>
            Enter();

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }
}

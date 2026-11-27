// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Diagnostics.HealthChecks;
using WeAmp.PageSpeed.Native;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class HealthCheckTests
{
    [Fact]
    public async Task ReturnsUnhealthy_WhenCacheThrows()
    {
        // Arrange: cache GetStats() throws.
        var cache = new FakePageSpeedCache(
            throwOnGetStats: new InvalidOperationException("cache broken"));

        var services = new ServiceCollection();
        services.AddSingleton<IPageSpeedCache>(cache);
        var sp = services.BuildServiceProvider();

        var healthCheck = new PageSpeedHealthCheck(sp);

        // Act
        var result = await healthCheck.CheckHealthAsync(
            new HealthCheckContext
            {
                Registration = new HealthCheckRegistration(
                    "test", healthCheck, null, null),
            });

        // Assert
        Assert.Equal(HealthStatus.Unhealthy, result.Status);
        Assert.Contains("cache error", result.Description!,
            StringComparison.OrdinalIgnoreCase);
        Assert.NotNull(result.Exception);
    }

    [Fact]
    public async Task ReturnsUnhealthy_WhenCacheServiceNotRegistered()
    {
        // Arrange: no IPageSpeedCache registered in DI.
        var services = new ServiceCollection();
        var sp = services.BuildServiceProvider();

        var healthCheck = new PageSpeedHealthCheck(sp);

        // Act
        var result = await healthCheck.CheckHealthAsync(
            new HealthCheckContext
            {
                Registration = new HealthCheckRegistration(
                    "test", healthCheck, null, null),
            });

        // Assert: missing service -> exception -> Unhealthy.
        Assert.Equal(HealthStatus.Unhealthy, result.Status);
        Assert.NotNull(result.Exception);
    }

    [NativeLibAbsentFact]
    public async Task CacheStatsSucceeds_ButVersionCheckFails_WhenNativeLibMissing()
    {
        // Arrange: cache returns valid stats, but PageSpeedVersion.Current
        // will throw because the native library is not available.
        var stats = new CacheStats(
            RamCacheHits: 100, RamCacheMisses: 10,
            DiskCacheHits: 200, DiskCacheMisses: 20,
            BytesRead: 1024, BytesWritten: 512,
            Evictions: 5, CurrentEntries: 42,
            CurrentSizeBytes: 4096, VolumeCapacityBytes: 1073741824,
            RamCacheBytes: 67108864, TotalHits: 300, TotalMisses: 30);
        var cache = new FakePageSpeedCache(stats: stats);

        var services = new ServiceCollection();
        services.AddSingleton<IPageSpeedCache>(cache);
        var sp = services.BuildServiceProvider();

        var healthCheck = new PageSpeedHealthCheck(sp);

        // Act
        var result = await healthCheck.CheckHealthAsync(
            new HealthCheckContext
            {
                Registration = new HealthCheckRegistration(
                    "test", healthCheck, null, null),
            });

        // Assert: With the health check now catching version failures,
        // GetStats() succeeds and the result is Healthy with version "unknown".
        Assert.Equal(HealthStatus.Healthy, result.Status);
        Assert.NotNull(result.Data);
        Assert.Equal("unknown", result.Data["version"]);
        Assert.Equal(42UL, result.Data["entries"]);
    }

    // ── agent_optimize entitlement on /v1/health ──

    private static CacheStats SampleStats() => new(
        RamCacheHits: 1, RamCacheMisses: 0,
        DiskCacheHits: 1, DiskCacheMisses: 0,
        BytesRead: 1, BytesWritten: 1,
        Evictions: 0, CurrentEntries: 1,
        CurrentSizeBytes: 1, VolumeCapacityBytes: 1,
        RamCacheBytes: 1, TotalHits: 2, TotalMisses: 0);

    private static async Task<HealthCheckResult> RunHealthCheck(IPageSpeedCache cache)
    {
        var services = new ServiceCollection();
        services.AddSingleton(cache);
        var sp = services.BuildServiceProvider();
        var healthCheck = new PageSpeedHealthCheck(sp);
        return await healthCheck.CheckHealthAsync(
            new HealthCheckContext
            {
                Registration = new HealthCheckRegistration(
                    "test", healthCheck, null, null),
            });
    }

    [Fact]
    public async Task HealthData_ExposesAgentEntitledTrue_WhenWorkerEntitled()
    {
        var cache = new FakePageSpeedCache(stats: SampleStats(), agentEntitled: 1);

        var result = await RunHealthCheck(cache);

        Assert.Equal(HealthStatus.Healthy, result.Status);
        Assert.True((bool)result.Data["agent_optimize_entitled"]);
    }

    [Fact]
    public async Task HealthData_ExposesAgentEntitledFalse_WhenNotEntitled()
    {
        var cache = new FakePageSpeedCache(stats: SampleStats(), agentEntitled: 0);

        var result = await RunHealthCheck(cache);

        Assert.Equal(HealthStatus.Healthy, result.Status);
        Assert.False((bool)result.Data["agent_optimize_entitled"]);
    }

    [Fact]
    public async Task HealthData_AgentEntitledFalse_WhenEntitlementReadThrows()
    {
        // Fail-safe-closed: an entitlement read error must never advertise an
        // entitlement we can't actually read.
        var cache = new FakePageSpeedCache(
            stats: SampleStats(),
            throwOnAgentEntitled: new InvalidOperationException("read failed"));

        var result = await RunHealthCheck(cache);

        Assert.Equal(HealthStatus.Healthy, result.Status);
        Assert.False((bool)result.Data["agent_optimize_entitled"]);
    }

    /// <summary>
    /// Fake IPageSpeedCache that does not depend on the native library.
    /// NSubstitute cannot proxy IPageSpeedCache because of the internal
    /// SafeCacheHandle Handle property (Castle DynamicProxy limitation).
    /// </summary>
    private sealed class FakePageSpeedCache : IPageSpeedCache
    {
        private readonly CacheStats? _stats;
        private readonly Exception? _throwOnGetStats;
        private readonly int _agentEntitled;
        private readonly Exception? _throwOnAgentEntitled;

        public FakePageSpeedCache(
            CacheStats? stats = null,
            Exception? throwOnGetStats = null,
            int agentEntitled = 0,
            Exception? throwOnAgentEntitled = null)
        {
            _stats = stats;
            _throwOnGetStats = throwOnGetStats;
            _agentEntitled = agentEntitled;
            _throwOnAgentEntitled = throwOnAgentEntitled;
        }

        SafeCacheHandle IPageSpeedCache.Handle =>
            throw new NotSupportedException("Test fake has no native handle");

        public IReadResult? ReadBest(
            string url, string hostname, string scheme, uint mask) => null;

        public IReadResult? ReadBestAgent(
            string url, string hostname, string scheme, uint mask, int agentEntitled) => null;

        public int ReadSharedConfigAgentEntitled()
        {
            if (_throwOnAgentEntitled != null) throw _throwOnAgentEntitled;
            return _agentEntitled;
        }

        public IReadResult? ReadAlternate(
            string url, string hostname, string scheme, byte alternateId) => null;

        public IReadResult? ReadEarlyHints(
            string url, string hostname, string scheme) => null;

        public CacheWriter BeginWrite(
            string url, string hostname, string scheme, CacheWriteParams p) =>
            throw new NotSupportedException("Test fake does not support writes");

        public CacheWriter BeginWriteSentinel(
            string url, string hostname, string scheme, byte sentinelId,
            ulong contentLength) =>
            throw new NotSupportedException("Test fake does not support writes");

        public CacheStats GetStats()
        {
            if (_throwOnGetStats != null) throw _throwOnGetStats;
            return _stats!;
        }

        public void RecordServeHit(
            PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask) { }

        public void Dispose() { }
    }
}

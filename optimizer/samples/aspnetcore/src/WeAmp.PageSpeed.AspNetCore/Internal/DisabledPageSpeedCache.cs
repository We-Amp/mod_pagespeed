// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// Cache stand-in used while PageSpeed has not been enabled yet: an empty
/// cache that never touches the disk. <see cref="DeferredPageSpeedCache"/>
/// answers through it until <see cref="PageSpeedActivation"/> opens the real
/// cache.
/// </summary>
internal sealed class DisabledPageSpeedCache : IPageSpeedCache
{
    /// <summary>The shared instance; it holds no state.</summary>
    internal static readonly DisabledPageSpeedCache Instance = new();

    public void Dispose()
    {
    }

    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
        => null;

    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
        => null;

    public int ReadSharedConfigAgentEntitled() => 0;

    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
        => null;

    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
        => null;

    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams p)
        => throw new CacheNotOpenException();

    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId, ulong contentLength)
        => throw new CacheNotOpenException();

    public HeadersSidecarWriteResult WriteHeadersSidecar(
        string url, string hostname, string scheme,
        IReadOnlyList<KeyValuePair<string, string>> headers)
        => throw new CacheNotOpenException();

    public CacheStats GetStats() => new(
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
    {
    }

    SafeCacheHandle IPageSpeedCache.Handle => throw new CacheNotOpenException();
}

/// <summary>
/// Thrown by the stand-in for an operation that needs an open cache. It is
/// not open either because PageSpeed has not been enabled yet or because
/// opening the volume failed; the caller cannot tell which, so the message
/// names neither.
/// </summary>
internal sealed class CacheNotOpenException : InvalidOperationException
{
    public CacheNotOpenException()
        : base("The PageSpeed cache is not open.")
    {
    }
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore.Internal;

/// <summary>
/// The <see cref="IPageSpeedCache"/> registered by <c>AddPageSpeed</c>: it
/// answers as an empty cache (<see cref="DisabledPageSpeedCache"/>) until
/// <see cref="PageSpeedActivation"/> opens the real one, and forwards every
/// call to the real cache from then on.
/// </summary>
/// <remarks>
/// A call made while <see cref="PageSpeedOptions.Enabled"/> is true but the
/// options change notification has not activated PageSpeed yet activates it
/// on the spot, so the first request after a switch-on already uses the real
/// cache.
/// </remarks>
internal sealed class DeferredPageSpeedCache : IPageSpeedCache
{
    private readonly PageSpeedActivation _activation;

    internal DeferredPageSpeedCache(PageSpeedActivation activation)
    {
        _activation = activation;
    }

    /// <summary>Whether the real cache is open.</summary>
    internal bool IsActive => _activation.ActiveCache != null;

    /// <summary>
    /// Whether the real cache is open, activating first when
    /// <see cref="PageSpeedOptions.Enabled"/> is already true.
    /// </summary>
    internal bool TryEnsureActive() => _activation.TryActivateFromRequest() != null;

    /// <summary>
    /// Why the cache could not be opened after PageSpeed was enabled, or
    /// <c>null</c>.
    /// </summary>
    internal Exception? OpenFailure => _activation.OpenFailure;

    /// <summary>
    /// True once per failure state; see
    /// <see cref="PageSpeedActivation.TryClaimNotOpenReport"/>.
    /// </summary>
    internal bool TryClaimNotOpenReport() => _activation.TryClaimNotOpenReport();

    /// <summary>The real cache, or <c>null</c> while inactive (for tests).</summary>
    internal IPageSpeedCache? ActiveCache => _activation.ActiveCache;

    private IPageSpeedCache Target =>
        _activation.TryActivateFromRequest() ?? DisabledPageSpeedCache.Instance;

    /// <summary>
    /// The real cache's handle for a native call, or <c>null</c> while
    /// inactive: the caller then runs without a cache instead of failing.
    /// </summary>
    internal SafeCacheHandle? TryGetHandle() =>
        _activation.TryActivateFromRequest()?.Handle;

    public IReadResult? ReadBest(string url, string hostname, string scheme, uint mask)
        => Target.ReadBest(url, hostname, scheme, mask);

    public IReadResult? ReadBestAgent(
        string url, string hostname, string scheme, uint mask, int agentEntitled)
        => Target.ReadBestAgent(url, hostname, scheme, mask, agentEntitled);

    public int ReadSharedConfigAgentEntitled() => Target.ReadSharedConfigAgentEntitled();

    public IReadResult? ReadAlternate(
        string url, string hostname, string scheme, byte alternateId)
        => Target.ReadAlternate(url, hostname, scheme, alternateId);

    public IReadResult? ReadEarlyHints(string url, string hostname, string scheme)
        => Target.ReadEarlyHints(url, hostname, scheme);

    public CacheWriter BeginWrite(
        string url, string hostname, string scheme, CacheWriteParams p)
        => Target.BeginWrite(url, hostname, scheme, p);

    public CacheWriter BeginWriteSentinel(
        string url, string hostname, string scheme, byte sentinelId, ulong contentLength)
        => Target.BeginWriteSentinel(url, hostname, scheme, sentinelId, contentLength);

    public HeadersSidecarWriteResult WriteHeadersSidecar(
        string url, string hostname, string scheme,
        IReadOnlyList<KeyValuePair<string, string>> headers)
        => Target.WriteHeadersSidecar(url, hostname, scheme, headers);

    public CacheStats GetStats() => Target.GetStats();

    public void RecordServeHit(
        PageSpeedContentType type, uint originalBytes, ulong optimizedBytes, uint mask)
        => Target.RecordServeHit(type, originalBytes, optimizedBytes, mask);

    /// <summary>
    /// The real cache's handle. While inactive this throws; HtmlProcessor
    /// uses <see cref="TryGetHandle"/> instead.
    /// </summary>
    SafeCacheHandle IPageSpeedCache.Handle => Target.Handle;

    /// <summary>
    /// Disposes the real cache, if open (the activation owns it). Every later
    /// call throws <see cref="ObjectDisposedException"/>, and no cache is
    /// opened again.
    /// </summary>
    public void Dispose() => _activation.Dispose();
}

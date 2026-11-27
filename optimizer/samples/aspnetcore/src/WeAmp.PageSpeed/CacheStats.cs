// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>Aggregate cache statistics (immutable snapshot).</summary>
public sealed record CacheStats(
    ulong RamCacheHits, ulong RamCacheMisses,
    ulong DiskCacheHits, ulong DiskCacheMisses,
    ulong BytesRead, ulong BytesWritten,
    ulong Evictions, ulong CurrentEntries,
    ulong CurrentSizeBytes, ulong VolumeCapacityBytes,
    ulong RamCacheBytes, ulong TotalHits, ulong TotalMisses)
{
    internal static CacheStats FromNative(NativeCacheStats n) => new(
        n.RamCacheHits, n.RamCacheMisses,
        n.DiskCacheHits, n.DiskCacheMisses,
        n.BytesRead, n.BytesWritten,
        n.Evictions, n.CurrentEntries,
        n.CurrentSizeBytes, n.VolumeCapacityBytes,
        n.RamCacheBytes, n.TotalHits, n.TotalMisses);
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.ComponentModel.DataAnnotations;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Configuration for the Cyclone cache volume.
/// </summary>
public sealed class PageSpeedCacheOptions
{
    /// <summary>Path to the Cyclone cache volume file.</summary>
    [Required]
    public string VolumePath { get; set; } = "/var/cache/pagespeed/volume.dat";

    /// <summary>Volume size in bytes. Default: 1 GB.</summary>
    [Range(1, (double)ulong.MaxValue)]
    public ulong VolumeSizeBytes { get; set; } = 1_073_741_824;

    /// <summary>Whether to enable checksum verification on cache reads.</summary>
    public bool EnableChecksum { get; set; } = true;

    /// <summary>RAM cache size in bytes. Default: 64 MB.</summary>
    [Range(0, long.MaxValue)]
    public long RamCacheSizeBytes { get; set; } = 67_108_864;

    /// <summary>Maximum metadata size in bytes. Default: 8 KB.</summary>
    [Range(0, long.MaxValue)]
    public long MaxMetadataSizeBytes { get; set; } = 8192;
}

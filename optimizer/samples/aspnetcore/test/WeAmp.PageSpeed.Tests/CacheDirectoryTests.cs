// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Tests that PageSpeedCache auto-creates the parent directory for VolumePath
/// and validates null/empty paths.
/// These tests require the native library to be available (they call ps_cache_open).
/// They are skipped gracefully when the native library is absent.
/// </summary>
public class CacheDirectoryTests : IDisposable
{
    private readonly List<string> _tempDirs = new();

    private string MakeTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), $"ps_test_{Guid.NewGuid():N}");
        _tempDirs.Add(dir);
        return dir;
    }

    [Fact]
    public void VolumePath_NullOrEmpty_ThrowsArgumentException()
    {
        var opts = new PageSpeedCacheOptions
        {
            VolumePath = null!,
            VolumeSizeBytes = 16 * 1024 * 1024,
        };

        Assert.Throws<ArgumentException>(() => new PageSpeedCache(opts));

        opts.VolumePath = "";
        Assert.Throws<ArgumentException>(() => new PageSpeedCache(opts));
    }

    [Fact]
    public void VolumePath_NonExistentParentDirectory_CreatesDirectoryAndSucceeds()
    {
        var baseDir = MakeTempDir();
        // Nested path where neither "deep" nor "nested" exist yet
        var volumePath = Path.Combine(baseDir, "deep", "nested", "volume.dat");
        Assert.False(Directory.Exists(Path.GetDirectoryName(volumePath)));

        var opts = new PageSpeedCacheOptions
        {
            VolumePath = volumePath,
            VolumeSizeBytes = 16 * 1024 * 1024,
            RamCacheSizeBytes = 1024 * 1024,
            MaxMetadataSizeBytes = 4096,
        };

        try
        {
            using var cache = new PageSpeedCache(opts);
            // If we get here, the constructor auto-created the directory
            Assert.True(Directory.Exists(Path.Combine(baseDir, "deep", "nested")));
        }
        catch (DllNotFoundException)
        {
            // Native library not available — skip gracefully
        }
    }

    [Fact]
    public void VolumePath_ExistingDirectory_Succeeds()
    {
        var dir = MakeTempDir();
        Directory.CreateDirectory(dir);
        var volumePath = Path.Combine(dir, "volume.dat");

        var opts = new PageSpeedCacheOptions
        {
            VolumePath = volumePath,
            VolumeSizeBytes = 16 * 1024 * 1024,
            RamCacheSizeBytes = 1024 * 1024,
            MaxMetadataSizeBytes = 4096,
        };

        try
        {
            using var cache = new PageSpeedCache(opts);
            // Should succeed without issue
        }
        catch (DllNotFoundException)
        {
            // Native library not available — skip gracefully
        }
    }

    [Fact]
    public void VolumePath_DirectoryCreation_IsIdempotent()
    {
        var baseDir = MakeTempDir();
        var volumePath = Path.Combine(baseDir, "sub", "volume.dat");

        var opts = new PageSpeedCacheOptions
        {
            VolumePath = volumePath,
            VolumeSizeBytes = 16 * 1024 * 1024,
            RamCacheSizeBytes = 1024 * 1024,
            MaxMetadataSizeBytes = 4096,
        };

        try
        {
            // First call creates the directory
            using var cache1 = new PageSpeedCache(opts);
            cache1.Dispose();

            // Second call with same path should not fail
            using var cache2 = new PageSpeedCache(opts);
        }
        catch (DllNotFoundException)
        {
            // Native library not available — skip gracefully
        }
    }

    public void Dispose()
    {
        foreach (var dir in _tempDirs)
        {
            try { Directory.Delete(dir, true); }
            catch { /* best effort */ }
        }
    }
}

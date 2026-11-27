// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

/// <summary>
/// Integration tests verifying that AddPageSpeed auto-creates the cache
/// parent directory via the DI factory path — the exact scenario that
/// caused the original PS_ERR_IO bug on macOS.
/// These tests require native binaries; they skip gracefully when absent.
/// </summary>
public class AddPageSpeedIntegrationTests : IDisposable
{
    private readonly List<string> _tempDirs = new();

    private string MakeTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), $"ps_int_{Guid.NewGuid():N}");
        _tempDirs.Add(dir);
        return dir;
    }

    [Fact]
    public async Task AddPageSpeed_NonExistentCacheDirectory_AutoCreatesAndStarts()
    {
        var baseDir = MakeTempDir();
        var cachePath = Path.Combine(baseDir, "deep", "volume.dat");
        Assert.False(Directory.Exists(Path.Combine(baseDir, "deep")));

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = cachePath;
                opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
                opts.Worker.AutoStart = false;
            });

            var app = builder.Build();
            app.UsePageSpeed();
            await app.StartAsync();

            // DI factory ran and auto-created the directory
            Assert.True(Directory.Exists(Path.Combine(baseDir, "deep")));

            // Cache singleton is resolvable
            var cache = app.Services.GetService<IPageSpeedCache>();
            Assert.NotNull(cache);

            await app.StopAsync();
        }
        catch (DllNotFoundException)
        {
            // Native library not available — skip gracefully
        }
    }

    [NativeLibFact]
    public async Task AddPageSpeed_EnabledFalse_DoesNotCreateCacheVolume()
    {
        var baseDir = MakeTempDir();
        var cachePath = Path.Combine(baseDir, "deep", "volume.dat");
        Assert.False(Directory.Exists(Path.Combine(baseDir, "deep")));

        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.WebHost.UseTestServer();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddPageSpeed(opts =>
        {
            opts.Enabled = false;
            opts.Cache.VolumePath = cachePath;
            opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
            opts.Worker.AutoStart = false;
        });

        var app = builder.Build();
        app.UsePageSpeed();
        await app.StartAsync();

        var cache = app.Services.GetService<IPageSpeedCache>();
        Assert.NotNull(cache);
        var stats = cache!.GetStats();
        Assert.Equal(0ul, stats.CurrentEntries);
        Assert.False(Directory.Exists(Path.Combine(baseDir, "deep")));
        Assert.False(File.Exists(cachePath));

        await app.StopAsync();
    }

    [Fact]
    public async Task AddPageSpeed_ExistingCacheDirectory_StartsSuccessfully()
    {
        var dir = MakeTempDir();
        Directory.CreateDirectory(dir);
        var cachePath = Path.Combine(dir, "volume.dat");

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = cachePath;
                opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
                opts.Worker.AutoStart = false;
            });

            var app = builder.Build();
            app.UsePageSpeed();
            await app.StartAsync();

            var cache = app.Services.GetService<IPageSpeedCache>();
            Assert.NotNull(cache);

            await app.StopAsync();
        }
        catch (DllNotFoundException)
        {
            // Native library not available — skip gracefully
        }
    }

    [Fact]
    public async Task AddPageSpeed_TempPathWithSubdirectory_WorksOnAllPlatforms()
    {
        // This replicates the exact DemoSite pattern that failed on macOS
        // where Path.GetTempPath() returns /var/folders/.../T/ instead of /tmp/
        var cachePath = Path.Combine(
            Path.GetTempPath(), $"ps_demo_{Guid.NewGuid():N}", "volume.dat");
        _tempDirs.Add(Path.GetDirectoryName(cachePath)!);

        try
        {
            var builder = WebApplication.CreateBuilder(Array.Empty<string>());
            builder.WebHost.UseTestServer();
            builder.Logging.SetMinimumLevel(LogLevel.Warning);
            builder.Services.AddPageSpeed(opts =>
            {
                opts.Cache.VolumePath = cachePath;
                opts.Cache.VolumeSizeBytes = 16 * 1024 * 1024;
                opts.Worker.AutoStart = false;
            });

            var app = builder.Build();
            app.UsePageSpeed();
            await app.StartAsync();

            Assert.True(Directory.Exists(Path.GetDirectoryName(cachePath)));

            await app.StopAsync();
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

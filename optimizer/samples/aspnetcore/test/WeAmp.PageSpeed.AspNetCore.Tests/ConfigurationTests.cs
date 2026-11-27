// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Text;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Options;
using Xunit;

namespace WeAmp.PageSpeed.AspNetCore.Tests;

public class ConfigurationTests
{
    [Fact]
    public void PageSpeedOptions_HasCorrectDefaults()
    {
        var options = new PageSpeedOptions();

        Assert.True(options.Enabled);
        Assert.True(options.RecordResponses);
        Assert.Equal(5 * 1024 * 1024, options.MaxResponseBufferBytes);
        Assert.NotNull(options.Cache);
        Assert.NotNull(options.Html);
        Assert.NotNull(options.Worker);
        Assert.NotNull(options.ExcludePaths);
        Assert.Equal("PageSpeed", PageSpeedOptions.SectionName);
    }

    [Fact]
    public void PageSpeedOptions_DefaultExcludePaths()
    {
        var options = new PageSpeedOptions();

        Assert.Contains("/api/", options.ExcludePaths);
        Assert.Contains("/signalr/", options.ExcludePaths);
        Assert.Contains("/_blazor/", options.ExcludePaths);
        Assert.Contains("/_framework/", options.ExcludePaths);
        Assert.Equal(4, options.ExcludePaths.Count);
    }

    [Fact]
    public void HtmlProcessingOptions_HasCorrectDefaults()
    {
        var options = new HtmlProcessingOptions();

        Assert.True(options.EnableCriticalCss);
        Assert.True(options.EnableLazyLoad);
        Assert.True(options.EnableImageDimensions);
        Assert.True(options.EnableLcpPreload);
        Assert.True(options.EnablePreconnect);
        Assert.False(options.EnableSpeculationRules);
        Assert.Equal(PageSpeedViewport.Desktop, options.Viewport);
        Assert.Equal(5 * 1024 * 1024, options.MaxHtmlSizeBytes);
        Assert.Equal(2 * 1024 * 1024, options.MaxCssSizeBytes);
    }

    [Fact]
    public void WorkerOptions_HasCorrectDefaults()
    {
        var options = new WorkerOptions();

        Assert.True(options.AutoStart);
        // v2.0.14: the unset default now resolves to a real auto-path (not
        // null) when AutoStart is true, so worker notifications work out of
        // the box. A null default silently disabled the trial dashboard.
        Assert.NotNull(options.SocketPath);
        Assert.Equal(0, options.ApiPort);
        Assert.Null(options.LogLevel);
    }

    [Fact]
    public void WorkerOptions_UnsetSocketPath_ResolvesToAutoPathWhenAutoStart()
    {
        var options = new WorkerOptions();

        var path = options.SocketPath;
        Assert.NotNull(path);
        Assert.Contains("pagespeed-", path);
        Assert.EndsWith(".sock", path);
        // Stable across reads on the same instance (memoized).
        Assert.Equal(path, options.SocketPath);
    }

    [Fact]
    public void WorkerOptions_UnsetSocketPath_IsNullWhenAutoStartDisabled()
    {
        var options = new WorkerOptions { AutoStart = false };

        Assert.Null(options.SocketPath);
    }

    [Fact]
    public void WorkerOptions_ExplicitNullSocketPath_DisablesCoordination()
    {
        // Explicitly assigning null (in code or via "SocketPath": null in
        // config) must keep meaning "disabled", even though AutoStart is true.
        var options = new WorkerOptions { AutoStart = true, SocketPath = null };

        Assert.Null(options.SocketPath);
    }

    [Fact]
    public void WorkerOptions_ExplicitSocketPath_IsHonoredVerbatim()
    {
        var options = new WorkerOptions { SocketPath = "/run/custom.sock" };

        Assert.Equal("/run/custom.sock", options.SocketPath);
    }

    [Fact]
    public void WorkerOptions_ExplicitEmptySocketPath_DisablesCoordination()
    {
        // The net8.0 source-generated binder turns "SocketPath": null into an
        // empty string; an empty path is never usable, so it must collapse to
        // the disabled (null) state just like an explicit null.
        var options = new WorkerOptions { AutoStart = true, SocketPath = "" };

        Assert.Null(options.SocketPath);
    }

    // Resolve Worker.SocketPath through the SAME source-generated binder
    // (BindConfiguration + EnableConfigurationBindingGenerator=true) that
    // AddPageSpeed uses, so these lock in the load-bearing distinction
    // between an explicit JSON null (disabled) and an absent key (auto-path).
    private static string? ResolveSocketPathFromJson(string json)
    {
        var cfg = new ConfigurationBuilder()
            .AddJsonStream(new MemoryStream(Encoding.UTF8.GetBytes(json)))
            .Build();
        var services = new ServiceCollection();
        services.AddSingleton<IConfiguration>(cfg);
        services.AddOptions<PageSpeedOptions>()
            .BindConfiguration(PageSpeedOptions.SectionName);
        using var sp = services.BuildServiceProvider();
        return sp.GetRequiredService<IOptions<PageSpeedOptions>>()
            .Value.Worker.SocketPath;
    }

    [Fact]
    public void Binder_ExplicitNullSocketPath_DisablesCoordination()
    {
        var resolved = ResolveSocketPathFromJson(
            """{ "PageSpeed": { "Worker": { "SocketPath": null } } }""");

        Assert.Null(resolved);
    }

    [Fact]
    public void Binder_AbsentSocketPath_ResolvesToAutoPath()
    {
        var resolved = ResolveSocketPathFromJson(
            """{ "PageSpeed": { "Worker": { "LogLevel": "info" } } }""");

        Assert.NotNull(resolved);
        Assert.Contains("pagespeed-", resolved);
        Assert.EndsWith(".sock", resolved);
    }

    [Fact]
    public void Binder_ConcreteSocketPath_IsHonoredVerbatim()
    {
        var resolved = ResolveSocketPathFromJson(
            """{ "PageSpeed": { "Worker": { "SocketPath": "/run/x.sock" } } }""");

        Assert.Equal("/run/x.sock", resolved);
    }

    [Fact]
    public void Binder_RetiredLicenseKeys_StillBindAndAreIgnored()
    {
        // Existing appsettings may still carry the retired license settings.
        // They must keep binding without error and must not disturb the keys
        // around them; the middleware itself never reads them.
        var cfg = new ConfigurationBuilder()
            .AddJsonStream(new MemoryStream(Encoding.UTF8.GetBytes(
                """
                { "PageSpeed": {
                    "Enabled": true,
                    "LicenseKey": "retired-setting",
                    "Worker": {
                      "LicenseRenewalUrl": "https://retired.example",
                      "LogLevel": "info" } } }
                """)))
            .Build();
        var services = new ServiceCollection();
        services.AddSingleton<IConfiguration>(cfg);
        services.AddOptions<PageSpeedOptions>()
            .BindConfiguration(PageSpeedOptions.SectionName);
        using var sp = services.BuildServiceProvider();

        var options = sp.GetRequiredService<IOptions<PageSpeedOptions>>().Value;

        Assert.True(options.Enabled);
        Assert.Equal("info", options.Worker.LogLevel);
    }

    [Fact]
    public void PageSpeedCacheOptions_HasCorrectDefaults()
    {
        var options = new PageSpeedCacheOptions();

        Assert.Equal("/var/cache/pagespeed/volume.dat", options.VolumePath);
        Assert.Equal(1_073_741_824UL, options.VolumeSizeBytes);
        Assert.True(options.EnableChecksum);
        Assert.Equal(67_108_864L, options.RamCacheSizeBytes);
        Assert.Equal(8192L, options.MaxMetadataSizeBytes);
    }

    [Fact]
    public void PageSpeedOptions_CanModifyProperties()
    {
        var options = new PageSpeedOptions
        {
            Enabled = false,
            MaxResponseBufferBytes = 1024,
            ExcludePaths = ["/custom/"],
        };

        Assert.False(options.Enabled);
        Assert.Equal(1024, options.MaxResponseBufferBytes);
        Assert.Single(options.ExcludePaths);
        Assert.Equal("/custom/", options.ExcludePaths[0]);
    }

    [Fact]
    public void HtmlProcessingOptions_CanModifyProperties()
    {
        var options = new HtmlProcessingOptions
        {
            EnableCriticalCss = false,
            EnableLazyLoad = false,
            EnableImageDimensions = false,
            EnableLcpPreload = false,
            EnablePreconnect = false,
            EnableSpeculationRules = true,
            Viewport = PageSpeedViewport.Mobile,
            MaxHtmlSizeBytes = 1024,
            MaxCssSizeBytes = 512,
        };

        Assert.False(options.EnableCriticalCss);
        Assert.False(options.EnableLazyLoad);
        Assert.False(options.EnableImageDimensions);
        Assert.False(options.EnableLcpPreload);
        Assert.False(options.EnablePreconnect);
        Assert.True(options.EnableSpeculationRules);
        Assert.Equal(PageSpeedViewport.Mobile, options.Viewport);
        Assert.Equal(1024, options.MaxHtmlSizeBytes);
        Assert.Equal(512, options.MaxCssSizeBytes);
    }

    [Fact]
    public void PageSpeedCacheOptions_CanModifyProperties()
    {
        var options = new PageSpeedCacheOptions
        {
            VolumePath = "/tmp/test.dat",
            VolumeSizeBytes = 512 * 1024 * 1024UL,
            EnableChecksum = false,
            RamCacheSizeBytes = 32 * 1024 * 1024L,
            MaxMetadataSizeBytes = 4096L,
        };

        Assert.Equal("/tmp/test.dat", options.VolumePath);
        Assert.Equal(512 * 1024 * 1024UL, options.VolumeSizeBytes);
        Assert.False(options.EnableChecksum);
        Assert.Equal(32 * 1024 * 1024L, options.RamCacheSizeBytes);
        Assert.Equal(4096L, options.MaxMetadataSizeBytes);
    }
}

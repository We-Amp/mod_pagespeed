// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed;

/// <summary>
/// High-level HTML processing API. Thread-safe.
/// Rebuilds NativeHtmlConfig per call to support options hot-reload.
/// </summary>
public sealed class HtmlProcessor : IHtmlProcessor
{
    private readonly IPageSpeedCache? _cache;
    private readonly HtmlProcessingOptions _options;

    public HtmlProcessor(
        IPageSpeedCache? cache,
        HtmlProcessingOptions options)
    {
        _cache = cache;
        _options = options;
    }

    public HtmlResult Process(
        ReadOnlySpan<byte> html, string url, string hostname) =>
        ProcessCore(html, url, hostname, null);

    public HtmlResult Process(
        ReadOnlySpan<byte> html, string url, string hostname, string scheme) =>
        ProcessCore(html, url, hostname, scheme);

    private HtmlResult ProcessCore(
        ReadOnlySpan<byte> html, string url, string hostname, string? scheme)
    {
        var config = BuildNativeConfig(_options);

        // NULL leaves the library's default ("https"). The UTF-8 copy stays
        // alive until the native call has returned.
        var schemePtr = scheme is null
            ? IntPtr.Zero
            : Marshal.StringToCoTaskMemUTF8(scheme);
        try
        {
            config.Scheme = schemePtr;
            int err;
            IntPtr raw;
            try
            {
                err = NativePageSpeed.HtmlProcess(
                    html, (nuint)html.Length,
                    url, hostname,
                    in config, _cache?.Handle,
                    out raw);
            }
            catch (ObjectDisposedException) when (_cache != null)
            {
                // A cache that swaps its instance (the ASP.NET Core middleware's,
                // after a volume reset) can close the handle between reading
                // Handle and the call (the call never started); Handle now
                // returns the replacement.
                err = NativePageSpeed.HtmlProcess(
                    html, (nuint)html.Length,
                    url, hostname,
                    in config, _cache.Handle,
                    out raw);
            }
            NativeCheck.ThrowOnError(err);
            return new HtmlResult(new SafeHtmlResultHandle(raw));
        }
        finally
        {
            if (schemePtr != IntPtr.Zero) Marshal.FreeCoTaskMem(schemePtr);
        }
    }

    private static NativeHtmlConfig BuildNativeConfig(
        HtmlProcessingOptions opts)
    {
        var config = new NativeHtmlConfig();
        NativePageSpeed.HtmlConfigInit(ref config);
        // HtmlConfigInit stamps the 128-byte size; claim the whole struct so
        // the library reads Scheme.
        config.StructSize = (nuint)Marshal.SizeOf<NativeHtmlConfig>();
        config.EnableCriticalCss = opts.EnableCriticalCss ? 1 : 0;
        config.EnableLazyLoad = opts.EnableLazyLoad ? 1 : 0;
        config.EnableImageDimensions = opts.EnableImageDimensions ? 1 : 0;
        config.EnableLcpPreload = opts.EnableLcpPreload ? 1 : 0;
        config.EnablePreconnect = opts.EnablePreconnect ? 1 : 0;
        config.EnableSpeculationRules = opts.EnableSpeculationRules ? 1 : 0;
        config.EnableAsyncCss = opts.EnableAsyncCss ? 1 : 0;
        config.Viewport = (int)opts.Viewport;
        config.MaxHtmlSize = (nuint)opts.MaxHtmlSizeBytes;
        config.MaxCssSize = (nuint)opts.MaxCssSizeBytes;
        // Note: Selector pattern arrays (AlwaysIncludeSelectors etc.) require
        // PinnedStringArray marshalling. Supported via the low-level
        // HtmlTransform API. The high-level HtmlProcess sets them to
        // IntPtr.Zero (use defaults from ps_html_config_init).
        return config;
    }
}

/// <summary>HTML processing configuration options.</summary>
public sealed class HtmlProcessingOptions
{
    public bool EnableCriticalCss { get; set; } = true;
    public bool EnableLazyLoad { get; set; } = true;
    public bool EnableImageDimensions { get; set; } = true;
    public bool EnableLcpPreload { get; set; } = true;
    public bool EnablePreconnect { get; set; } = true;
    public bool EnableSpeculationRules { get; set; }

    /// <summary>
    /// Defer render-blocking stylesheets via a rel="preload" as="style" swap + a CSP-safe
    /// external loader (served at a content-hashed path under /pagespeed_static/,
    /// e.g. /pagespeed_static/async_css.&lt;hash&gt;.js) with a
    /// &lt;noscript&gt; fallback. Only fires when critical CSS is inlined. Opt-in;
    /// direct library consumers must serve the loader JS themselves
    /// (NativePageSpeed.AsyncCssLoaderPath()/AsyncCssLoaderJs()).
    /// </summary>
    public bool EnableAsyncCss { get; set; }
    public PageSpeedViewport Viewport { get; set; } = PageSpeedViewport.Desktop;
    public int MaxHtmlSizeBytes { get; set; } = 5 * 1024 * 1024;
    public int MaxCssSizeBytes { get; set; } = 2 * 1024 * 1024;
}

/// <summary>Cache configuration options.</summary>
public sealed class PageSpeedCacheOptions
{
    /// <summary>Path to the Cyclone cache volume file.</summary>
    public string VolumePath { get; set; } =
        "/var/cache/pagespeed/volume.dat";

    /// <summary>Volume size in bytes. Default: 1 GB.</summary>
    public ulong VolumeSizeBytes { get; set; } = 1_073_741_824;

    public bool EnableChecksum { get; set; } = true;

    public long RamCacheSizeBytes { get; set; } = 67_108_864;

    public long MaxMetadataSizeBytes { get; set; } = 8192;
}

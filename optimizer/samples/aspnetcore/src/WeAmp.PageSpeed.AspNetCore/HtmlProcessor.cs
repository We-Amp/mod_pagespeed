// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using System.Runtime.InteropServices;
using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// High-level HTML processing API. Thread-safe.
/// Rebuilds NativeHtmlConfig per call to support options hot-reload.
/// </summary>
public sealed class HtmlProcessor : IHtmlProcessor
{
    private readonly IPageSpeedCache? _cache;
    private readonly IOptionsMonitor<PageSpeedOptions> _options;

    /// <summary>
    /// Creates a new HTML processor.
    /// </summary>
    /// <param name="cache">
    /// Optional cache for CSS lookup during processing.
    /// </param>
    /// <param name="options">
    /// Options monitor for hot-reloadable configuration.
    /// </param>
    public HtmlProcessor(
        IPageSpeedCache? cache,
        IOptionsMonitor<PageSpeedOptions> options)
    {
        _cache = cache;
        _options = options;
    }

    /// <inheritdoc/>
    public HtmlResult Process(
        ReadOnlySpan<byte> html, string url, string hostname) =>
        ProcessCore(html, url, hostname, null);

    /// <inheritdoc/>
    public HtmlResult Process(
        ReadOnlySpan<byte> html, string url, string hostname, string scheme) =>
        ProcessCore(html, url, hostname, scheme);

    private HtmlResult ProcessCore(
        ReadOnlySpan<byte> html, string url, string hostname, string? scheme)
    {
        var opts = _options.CurrentValue.Html;
        var config = BuildNativeConfig(opts);

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
                    in config, CacheHandle(),
                    out raw);
            }
            catch (ObjectDisposedException) when (_cache != null)
            {
                // The cache instance was retired by a volume reset between reading
                // Handle and the call (the call never started); Handle now returns
                // the replacement.
                err = NativePageSpeed.HtmlProcess(
                    html, (nuint)html.Length,
                    url, hostname,
                    in config, CacheHandle(),
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

    // A handle that is never opened: the native call receives a null cache
    // (ps_html_process accepts one), which a null SafeHandle cannot express
    // (the marshaller dereferences it). Invalid, so it is never closed.
    private static readonly SafeCacheHandle NoCacheHandle = new();

    /// <summary>
    /// The cache handle for the native call; <see cref="NoCacheHandle"/> runs
    /// it without a cache. The cache <c>AddPageSpeed</c> registers is empty
    /// until PageSpeed is first enabled; until then HTML is processed as with
    /// no cache at all.
    /// </summary>
    private SafeCacheHandle CacheHandle() => _cache switch
    {
        null => NoCacheHandle,
        DeferredPageSpeedCache deferred => deferred.TryGetHandle() ?? NoCacheHandle,
        var cache => cache.Handle,
    };

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
        // HtmlTransform API. The high-level HtmlProcess sets them to IntPtr.Zero
        // (use defaults from ps_html_config_init).
        return config;
    }
}

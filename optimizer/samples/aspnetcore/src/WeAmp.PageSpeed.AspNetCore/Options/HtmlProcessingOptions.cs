// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Configuration for HTML processing features.
/// Supports hot-reload via IOptionsMonitor.
/// </summary>
public sealed class HtmlProcessingOptions
{
    /// <summary>Enable critical CSS extraction and inlining.</summary>
    public bool EnableCriticalCss { get; set; } = true;

    /// <summary>Enable lazy loading for below-the-fold images.</summary>
    public bool EnableLazyLoad { get; set; } = true;

    /// <summary>Enable explicit width/height on images.</summary>
    public bool EnableImageDimensions { get; set; } = true;

    /// <summary>Enable LCP image preload hint generation.</summary>
    public bool EnableLcpPreload { get; set; } = true;

    /// <summary>Enable preconnect hints for third-party origins.</summary>
    public bool EnablePreconnect { get; set; } = true;

    /// <summary>Enable speculation rules for prefetching.</summary>
    public bool EnableSpeculationRules { get; set; } = false;

    /// <summary>
    /// Defer render-blocking stylesheets to non-blocking loading (a
    /// rel="preload" as="style" swap + a CSP-safe external loader served at a content-hashed path under
    /// /pagespeed_static/, e.g. /pagespeed_static/async_css.&lt;hash&gt;.js),
    /// keeping a &lt;noscript&gt; fallback. Requires critical CSS (only fires when
    /// critical CSS was inlined).
    /// Off by default in the in-process middleware (opt-in), because the host
    /// must also serve the loader path. NOTE: the nginx/worker front-end ENABLES
    /// this by default whenever critical CSS is present (disable with
    /// --no-async-css); the C library / ps_html_config_init surface defaults it
    /// off. Enable here for parity with a default nginx deployment.
    /// </summary>
    public bool EnableAsyncCss { get; set; } = false;

    /// <summary>Enable script deferral based on browser coverage analysis.</summary>
    public bool EnableScriptDeferral { get; set; } = true;

    /// <summary>Default viewport for HTML processing.</summary>
    public PageSpeedViewport Viewport { get; set; } = PageSpeedViewport.Desktop;

    /// <summary>Maximum HTML size to process (bytes). Default: 5 MB.</summary>
    public int MaxHtmlSizeBytes { get; set; } = 5 * 1024 * 1024;

    /// <summary>Maximum CSS size to process (bytes). Default: 2 MB.</summary>
    public int MaxCssSizeBytes { get; set; } = 2 * 1024 * 1024;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// Top-level configuration for the PageSpeed middleware.
/// Bound to the "PageSpeed" configuration section.
/// </summary>
public sealed class PageSpeedOptions
{
    /// <summary>Configuration section name for IConfiguration binding.</summary>
    public const string SectionName = "PageSpeed";

    /// <summary>
    /// Whether the middleware is active. Supports hot-reload.
    /// The cache volume is opened, and the worker launched, the first time
    /// this is true, at startup or through a later hot reload. Switching it
    /// back to false leaves both in place.
    /// </summary>
    public bool Enabled { get; set; } = true;

    /// <summary>Cache volume configuration.</summary>
    public PageSpeedCacheOptions Cache { get; set; } = new();

    /// <summary>HTML processing configuration.</summary>
    public HtmlProcessingOptions Html { get; set; } = new();

    /// <summary>Worker notification configuration.</summary>
    public WorkerOptions Worker { get; set; } = new();

    /// <summary>
    /// Embedded /console/ SPA and worker-proxy configuration.
    /// </summary>
    public ConsoleOptions Console { get; set; } = new();

    /// <summary>
    /// URL path prefixes to exclude. Supports hot-reload.
    /// Default: ["/api/", "/signalr/", "/_blazor/", "/_framework/"]
    /// </summary>
    public List<string> ExcludePaths { get; set; } =
        ["/api/", "/signalr/", "/_blazor/", "/_framework/"];

    /// <summary>
    /// Whether the middleware stores responses in its cache. Supports
    /// hot-reload. On a cache miss whose response would be stored, the
    /// middleware requests the same URL once more as an anonymous request and
    /// stores that response, so the app handles each stored response twice.
    /// Set to false for an app whose GET handlers have side effects: pages
    /// are still optimized for each response, and nothing new is stored.
    /// Default: true.
    /// </summary>
    public bool RecordResponses { get; set; } = true;

    /// <summary>
    /// Max response body size to buffer (bytes). Responses larger than this
    /// pass through unmodified. Default: 5 MB.
    /// </summary>
    public int MaxResponseBufferBytes { get; set; } = 5 * 1024 * 1024;

    /// <summary>
    /// Max asset (image/CSS/JS) size to cache (bytes). Assets larger than
    /// this pass through without caching or worker notification.
    /// Default: 10 MB.
    /// </summary>
    public int MaxAssetCacheBytes { get; set; } = 10 * 1024 * 1024;

    /// <summary>
    /// Cache mode controlling Cache-Control headers on cache HIT responses.
    /// Safe (default): must-revalidate on assets, no-cache on HTML.
    /// Aggressive: public + stale-if-error on assets, no-cache on HTML.
    /// </summary>
    public CacheMode CacheMode { get; set; } = CacheMode.Safe;

    /// <summary>
    /// Max-age (seconds) for CSS and JS cache HIT responses.
    /// Default: 300 (5 minutes).
    /// </summary>
    public int CssMaxAgeSeconds { get; set; } = 300;

    /// <summary>
    /// Max-age (seconds) for image cache HIT responses.
    /// Default: 1800 (30 minutes).
    /// </summary>
    public int ImageMaxAgeSeconds { get; set; } = 1800;

    /// <summary>
    /// Max-age (seconds) for HTML cache HIT responses.
    /// Default: 0. HTML always receives no-cache regardless of this value.
    /// Reserved for future use.
    /// </summary>
    public int HtmlMaxAgeSeconds { get; set; } = 0;

    /// <summary>
    /// No longer used; the setting is ignored. Kept so existing configuration
    /// keeps binding.
    /// </summary>
    [Obsolete("No longer used; the setting is ignored.")]
    public string? LicenseKey { get; set; }

    /// <summary>
    /// Enable serving rendered-DOM markdown to entitled agent requests
    /// (<c>Accept: text/markdown</c>). OFF by default. This is the operator
    /// kill-switch only — even when true, serving still requires the
    /// worker-written entitlement AND the native content-hash binding gate, so
    /// markdown never reaches a non-entitled or non-markdown request.
    /// </summary>
    public bool EnableAgentOptimize { get; set; } = false;
}

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Root configuration options for PageSpeed middleware.
/// </summary>
public class PageSpeedOptions
{
    /// <summary>
    /// Configuration section name in appsettings.json.
    /// </summary>
    public const string SectionName = "PageSpeed";

    /// <summary>
    /// Whether PageSpeed optimization is enabled. Default: true.
    /// </summary>
    public bool Enabled { get; set; } = true;

    /// <summary>
    /// Rewrite level: PassThrough, CoreFilters, OptimizeForBandwidth, MobilizeFilters,
    /// TestingCoreFilters, AllFilters (the six canonical spellings the validator accepts).
    /// Default: CoreFilters.
    /// </summary>
    public string RewriteLevel { get; set; } = "CoreFilters";

    /// <summary>
    /// Enable specific filters (comma-separated filter names).
    /// Example: "rewrite_css,rewrite_javascript,recompress_images"
    /// </summary>
    public string? EnabledFilters { get; set; }

    /// <summary>
    /// Disable specific filters (comma-separated filter names).
    /// Disabled filters override enabled filters.
    /// </summary>
    public string? DisabledFilters { get; set; }

    /// <summary>
    /// URL path patterns to exclude from PageSpeed processing.
    /// Supports regex patterns. Example: ["^/api/", "^/signalr/"]
    /// </summary>
    public List<string> ExcludePaths { get; set; } = [];

    /// <summary>
    /// Sidecar process configuration.
    /// </summary>
    public SidecarOptions Sidecar { get; set; } = new();

    /// <summary>
    /// Cache configuration (in-memory and file cache).
    /// </summary>
    public CacheOptions Cache { get; set; } = new();

    /// <summary>
    /// Redis cache backend configuration (optional). PREVIEW: the nginx sidecar does
    /// not yet emit a <c>RedisServer</c> directive — when set it is ignored (with a
    /// startup warning) and the file cache is used. Use
    /// <see cref="CustomOptions"/> for directives not yet modeled.
    /// </summary>
    public RedisOptions? Redis { get; set; }

    /// <summary>
    /// Domain authorization and mapping configuration.
    /// </summary>
    public DomainOptions Domains { get; set; } = new();

    /// <summary>
    /// Admin endpoint authentication configuration.
    /// </summary>
    public AdminAuthOptions AdminAuth { get; set; } = new();

    /// <summary>
    /// Per-virtual-host configuration overrides.
    /// </summary>
    public List<VirtualHostOptions> VirtualHosts { get; set; } = [];

    /// <summary>
    /// Custom PageSpeed options as key-value pairs.
    /// Keys are PageSpeed directive names (e.g., "CssInlineMaxBytes").
    /// </summary>
    public Dictionary<string, string> CustomOptions { get; set; } = [];

    /// <summary>
    /// Retired setting from 2.0; accepted and ignored.
    /// </summary>
    [Obsolete("No longer used; the setting is ignored.")]
    public string? LicenseKey { get; set; }

    /// <summary>
    /// Retired setting from 2.0; accepted and ignored.
    /// </summary>
    [Obsolete("No longer used; the setting is ignored.")]
    public string? LicenseServiceUrl { get; set; }
}

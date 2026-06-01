namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Per-virtual-host PageSpeed configuration override.
/// Matches requests by host pattern and applies custom options.
/// </summary>
public class VirtualHostOptions
{
    /// <summary>
    /// Host pattern to match against the Host header.
    /// Supports wildcards: * matches any sequence, ? matches single character.
    /// Examples: "www.example.com", "*.example.com", "api-?.example.com"
    /// </summary>
    public string HostPattern { get; set; } = string.Empty;

    /// <summary>
    /// Priority for matching (lower = higher priority). Default: 0.
    /// When multiple patterns match, the one with lowest priority wins.
    /// </summary>
    public int Priority { get; set; } = 0;

    /// <summary>
    /// Whether PageSpeed is enabled for this host. Default: true.
    /// Set to false to disable all optimization for matching hosts.
    /// </summary>
    public bool Enabled { get; set; } = true;

    /// <summary>
    /// Rewrite level: PassThrough, CoreFilters, MobilizeFilters, TestingCoreFilters, AllFilters.
    /// </summary>
    public string? RewriteLevel { get; set; }

    /// <summary>
    /// Enable specific filters (comma-separated filter names).
    /// </summary>
    public string? EnabledFilters { get; set; }

    /// <summary>
    /// Disable specific filters (comma-separated filter names).
    /// </summary>
    public string? DisabledFilters { get; set; }

    /// <summary>
    /// Custom PageSpeed options as key-value pairs.
    /// Keys are PageSpeed directive names (e.g., "CssInlineMaxBytes").
    /// </summary>
    public Dictionary<string, string> CustomOptions { get; set; } = [];

    /// <summary>
    /// Domain configuration specific to this virtual host.
    /// Merged with global domain configuration.
    /// </summary>
    public DomainOptions? Domains { get; set; }
}

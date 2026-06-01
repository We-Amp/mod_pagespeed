namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Configuration for PageSpeed caching.
/// </summary>
public class CacheOptions
{
    /// <summary>
    /// In-memory LRU cache size in KB per process.
    /// Default: 512000 (500 MB).
    /// </summary>
    public long LruCacheSizeKb { get; set; } = 512000;

    /// <summary>
    /// File cache path. If not set, uses system temp directory.
    /// </summary>
    public string? FileCachePath { get; set; }

    /// <summary>
    /// File cache size limit in KB. Default: 10240000 (10 GB).
    /// </summary>
    public long FileCacheSizeKb { get; set; } = 10240000;

    /// <summary>
    /// Log directory. If not set, uses system temp directory.
    /// </summary>
    public string? LogDirectory { get; set; }
}

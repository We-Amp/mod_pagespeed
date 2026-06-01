namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Admin endpoint authentication configuration.
/// Controls access to /pagespeed_admin and /pagespeed_statistics.
/// </summary>
public class AdminAuthOptions
{
    /// <summary>
    /// Whether admin authentication is enabled. Default: true.
    /// </summary>
    public bool Enabled { get; set; } = true;

    /// <summary>
    /// Bearer token for authentication. If not set, a random token is generated.
    /// Clients must provide this in the Authorization header:
    /// "Authorization: Bearer {token}"
    /// </summary>
    public string? Token { get; set; }

    /// <summary>
    /// Allowed IP addresses/ranges in CIDR notation.
    /// If non-empty, requests must originate from these IPs.
    /// Default: ["127.0.0.1/32", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"]
    /// </summary>
    public List<string> AllowedIps { get; set; } =
    [
        "127.0.0.1/32",
        "10.0.0.0/8",
        "172.16.0.0/12",
        "192.168.0.0/16"
    ];

    /// <summary>
    /// Rate limit in requests per minute. Default: 60.
    /// Set to 0 to disable rate limiting.
    /// </summary>
    public int RateLimitRpm { get; set; } = 60;
}

namespace WeAmp.PageSpeed.AspNetCore.Options;

/// <summary>
/// Domain configuration for authorization and URL mapping.
/// </summary>
public class DomainOptions
{
    /// <summary>
    /// Domains authorized for CROSS-ORIGIN rewriting (e.g. a CDN or a separate asset
    /// host). Emitted as <c>pagespeed Domain &lt;d&gt;</c>. A request's OWN same-origin
    /// resources are ALWAYS authorized by the module without listing the serving host
    /// here, so this is NOT required for normal optimization. In the Inverse sidecar
    /// the middleware does NOT gate on this list by default (forward-all); it is the
    /// strict allowlist only when <see cref="SidecarOptions.RestrictToAuthorizedHosts"/>
    /// is enabled. Wildcards (*, ?) allowed. Example: ["cdn.example.com"]. The
    /// [localhost, 127.0.0.1] default is a harmless seed (inert for same-origin).
    /// </summary>
    public List<string> AuthorizedDomains { get; set; } = ["localhost", "127.0.0.1"];

    /// <summary>
    /// CDN domain mappings (rewrite URLs to use CDN). PREVIEW: not yet emitted by the
    /// nginx sidecar — when set it is ignored with a startup warning.
    /// </summary>
    public List<RewriteDomainMapping> RewriteMappings { get; set; } = [];

    /// <summary>
    /// Origin domain mappings (fetch from different origin). PREVIEW: not yet emitted
    /// by the nginx sidecar — when set it is ignored with a startup warning.
    /// </summary>
    public List<OriginDomainMapping> OriginMappings { get; set; } = [];

    /// <summary>
    /// Domain sharding configuration. PREVIEW: not yet emitted by the nginx sidecar —
    /// when set it is ignored with a startup warning.
    /// </summary>
    public List<DomainShard> Shards { get; set; } = [];
}

/// <summary>
/// Maps resources from one domain to another for CDN integration.
/// </summary>
public class RewriteDomainMapping
{
    /// <summary>
    /// Target domain for rewritten URLs (e.g., "cdn.example.com").
    /// </summary>
    public string ToDomain { get; set; } = string.Empty;

    /// <summary>
    /// Source domains, comma-separated. Wildcards allowed.
    /// Example: "www.example.com,*.example.com"
    /// </summary>
    public string FromDomains { get; set; } = string.Empty;
}

/// <summary>
/// Maps requests to a different origin for fetching.
/// </summary>
public class OriginDomainMapping
{
    /// <summary>
    /// Origin domain to fetch from (e.g., "localhost:8080").
    /// </summary>
    public string ToDomain { get; set; } = string.Empty;

    /// <summary>
    /// Source domains, comma-separated. Wildcards allowed.
    /// </summary>
    public string FromDomains { get; set; } = string.Empty;

    /// <summary>
    /// Host header to use when fetching. If empty, uses the from_domain.
    /// </summary>
    public string? HostHeader { get; set; }
}

/// <summary>
/// Domain sharding configuration for browser download parallelism.
/// </summary>
public class DomainShard
{
    /// <summary>
    /// Base domain to shard.
    /// </summary>
    public string Domain { get; set; } = string.Empty;

    /// <summary>
    /// Shard domains, comma-separated (e.g., "s1.example.com,s2.example.com").
    /// </summary>
    public string Shards { get; set; } = string.Empty;
}

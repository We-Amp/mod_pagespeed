using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Logging;
using WeAmp.PageSpeed.AspNetCore.Internal;
using WeAmp.PageSpeed.AspNetCore.Options;

namespace WeAmp.PageSpeed.AspNetCore.Config;

/// <summary>
/// Generates a complete, self-contained <c>nginx.conf</c> (with ngx_pagespeed
/// directives) from <see cref="PageSpeedOptions"/>. This is the nginx-sidecar
/// equivalent of the Envoy generator: the catch-all
/// <c>pagespeed &lt;Option&gt; &lt;value&gt;;</c> directive form, the three required
/// handler <c>location</c> blocks, the generated <c>/pagespeed/health</c> stub,
/// a loopback+bearer admin gate, and a <c>proxy_pass</c> to the Kestrel origin
/// resolved via the shared <see cref="InternalSidecarEndpoint"/>.
///
/// SECURITY (D8): every value that reaches the generated config is validated
/// fail-closed by a TYPED per-field allowlist — a crafted option value can never
/// break out of its directive into a new <c>server{}</c>/directive, and
/// <see cref="PageSpeedOptions.CustomOptions"/> is default-deny (only the curated
/// safe tuning directives are emitted; admin/path/fetch/script directives are
/// rejected). The directive allowlist is a reviewed security artifact pinned to
/// the bundled mod_pagespeed source (an unknown key fails nginx config-load =
/// fail-safe; see the design record P1).
/// </summary>
public class NginxConfigGenerator
{
    private readonly ILogger<NginxConfigGenerator> _logger;
    private readonly InternalSidecarEndpoint _endpoint;

    public NginxConfigGenerator(ILogger<NginxConfigGenerator> logger, InternalSidecarEndpoint endpoint)
    {
        _logger = logger;
        _endpoint = endpoint;
    }

    // The five admin/diagnostic handler paths the sidecar exposes. The generated
    // directive value AND the generated location/ACL path MUST stay in lockstep.
    private static readonly (string Directive, string Path)[] AdminHandlers =
    {
        ("AdminPath", "/pagespeed_admin"),
        ("StatisticsPath", "/pagespeed_statistics"),
        ("GlobalStatisticsPath", "/pagespeed_global_statistics"),
        ("MessagesPath", "/pagespeed_message"),
        ("ConsolePath", "/pagespeed_console"),
    };

    /// <summary>
    /// Drop-in replacement for the Envoy generator's method of the same name:
    /// writes the full nginx.conf to <paramref name="configPath"/> and returns
    /// the admin bearer token (generated if not configured). The only method
    /// <see cref="Sidecar.ProcessSidecarManager"/> calls.
    /// </summary>
    public string GenerateConfigFile(PageSpeedOptions options, string configPath, string? resolvedBinaryPath = null)
    {
        var prefix = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        Directory.CreateDirectory(prefix);

        var (conf, adminToken) = GenerateConfig(options, prefix, resolvedBinaryPath);

        File.WriteAllText(configPath, conf);
        // The generated conf embeds the admin bearer token -> 0600 (D8).
        TrySetMode(configPath, UnixFileMode.UserRead | UnixFileMode.UserWrite);

        _logger.LogInformation("Generated nginx configuration at {ConfigPath}", configPath);
        return adminToken;
    }

    /// <summary>
    /// Pure-string variant for tests: returns the generated conf and the admin
    /// token. Cache/log dirs are pre-created. The proxy origin
    /// is read from <see cref="InternalSidecarEndpoint"/>, which must already be
    /// pinned (the Kestrel configurator does this at host build).
    /// </summary>
    public (string Conf, string AdminToken) GenerateConfig(
        PageSpeedOptions options, string prefix, string? resolvedBinaryPath = null)
    {
        var adminToken = TokenSafe(options.AdminAuth.Token, "AdminAuth.Token") ?? GenerateRandomToken();

        WarnUnmappedOptions(options);

        var cacheDir = ResolveCacheDir(options, prefix);
        var logDir = ResolveLogDir(options, prefix);
        var modulePath = ResolveModulePath(options, resolvedBinaryPath);

        // Validate the attacker-influenced path inputs BEFORE creating anything,
        // so malicious path input can't even create a junk directory (fail-closed).
        RejectInjection(prefix, "ConfigDirectory");
        RejectInjection(modulePath, "ModulePath");
        RejectInjection(cacheDir, "FileCachePath");
        RejectInjection(logDir, "LogDir");

        Directory.CreateDirectory(cacheDir);
        Directory.CreateDirectory(logDir);
        TrySetMode(cacheDir, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        TrySetMode(logDir, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);

        // The Kestrel origin the sidecar reverse-proxies to (UDS or loopback);
        // throws if Kestrel was not configured first (fail-fast, surfaced).
        var proxyTarget = _endpoint.ProxyPassTarget();

        var sb = new StringBuilder();

        // ---- main scope ----
        sb.AppendLine($"load_module {modulePath};");
        // When the nginx master runs as root, its workers default to the
        // compiled-in 'nobody' user and can't write the (owner-only) cache dir.
        // Align workers to the cache owner. In production the sidecar runs
        // NON-root (D8), where nginx neither can nor needs to set 'user'.
        if (Environment.UserName == "root")
        {
            // Running the host as root makes nginx workers — which terminate untrusted
            // public traffic and run the optimizer C++ on attacker-supplied bodies —
            // run as root too (the 0700 cache is root-owned, so the compiled-in
            // unprivileged worker user cannot write it). the design record D8 prescribes a
            // non-root deployment; warn loudly so this isn't a silent loss of the
            // standard nginx privilege-drop.
            _logger.LogWarning(
                "PageSpeed sidecar host is running as root: nginx workers (which process untrusted " +
                "traffic + run the optimizer) will also run as root, removing the standard privilege-drop. " +
                "the design record D8 strongly recommends running the host as a non-root user.");
            sb.AppendLine("user root;");
        }
        sb.AppendLine("worker_processes 1;");
        // NOTE: 'daemon off;' is passed via the launch flag -g (ProcessSidecarManager),
        // NOT emitted here — setting it in both places is a duplicate-directive error.
        sb.AppendLine($"pid {prefix}/nginx.pid;");
        sb.AppendLine($"error_log {logDir}/error.log info;");
        sb.AppendLine("events { worker_connections 1024; }");
        sb.AppendLine("http {");

        // Admin bearer gate (owner decision A / §10): a single `map` centralizes
        // the token to ONE line (rotation-friendly, no per-location drift). The
        // string compare is NOT constant-time, but that is accepted as
        // defense-in-depth — the `deny all` below shuts the network attacker out
        // regardless of timing, and the token is high-entropy + per-deployment.
        if (options.AdminAuth.Enabled)
        {
            // The map key "Bearer <64-hex>" is ~71 bytes, over nginx's default
            // map_hash_bucket_size (64) -> raise it so nginx can build the hash.
            sb.AppendLine("  map_hash_bucket_size 128;");
            sb.AppendLine("  map $http_authorization $admin_ok {");
            sb.AppendLine("    default 0;");
            sb.AppendLine($"    \"Bearer {adminToken}\" 1;");
            sb.AppendLine("  }");
        }

        // Admin rate-limit (SPEC-3 — AdminAuthOptions.RateLimitRpm): a per-client
        // limit_req zone enforced on the admin locations below. Declared in http
        // scope (limit_req_zone is http-level). 0 disables it. This turns a
        // previously-documented-but-unwired knob into a real control. NOTE on what it
        // throttles: the bearer `if ($admin_ok = 0) { return 403; }` runs in nginx's
        // REWRITE phase, which precedes limit_req's PREACCESS phase, so unauthenticated
        // requests are rejected before the limiter counts them (verified e2e:
        // no-bearer x30 -> 403x30, with-bearer x30 -> 200x10 + 503x20). The cap
        // therefore bounds the rate of AUTHENTICATED admin traffic — protecting the
        // expensive admin/stats handlers from a runaway or compromised loopback client
        // — rather than throttling pre-auth brute-force (which the loopback ACL +
        // bearer already deny cheaply). Defense-in-depth on top of those gates.
        if (options.AdminAuth.Enabled && options.AdminAuth.RateLimitRpm > 0)
        {
            sb.AppendLine(
                $"  limit_req_zone $binary_remote_addr zone=pagespeed_admin:1m rate={RateLimit(options.AdminAuth.RateLimitRpm)}r/m;");
        }

        // ---- http scope: process/global pagespeed options ----
        foreach (var (directive, path) in AdminHandlers)
            sb.AppendLine($"  pagespeed {directive} {path};");
        // Module-level admin gate, INDEPENDENT of nginx location selection. The
        // ngx_pagespeed admin dispatcher is a precontent-phase handler that routes
        // on the URI prefix regardless of which nginx location wins, and is
        // open-by-default — so a URI like /pagespeed_admin.pagespeed.<id> that
        // matches the ungated .pagespeed. resource regex would otherwise reach the
        // admin console with NO ACL. StrictAdminAccess on + no pagespeed-level Allow
        // = admit admin/stats handlers only from a validated loopback client IP
        // (system_rewrite_options.cc). The nginx allow/deny+bearer below is the 2nd
        // layer; the admin locations are emitted as case-insensitive regex ordered
        // before the .pagespeed. regex so neither it nor an ExcludePaths regex can
        // shadow that layer.
        sb.AppendLine("  pagespeed StrictAdminAccess on;");
        sb.AppendLine($"  pagespeed FileCachePath {cacheDir};");
        sb.AppendLine($"  pagespeed LogDir {logDir};");
        if (options.Cache.FileCacheSizeKb > 0)
            sb.AppendLine($"  pagespeed FileCacheSizeKb {NonNegative(options.Cache.FileCacheSizeKb, "FileCacheSizeKb")};");
        if (options.Cache.LruCacheSizeKb > 0)
            sb.AppendLine($"  pagespeed LRUCacheKbPerProcess {NonNegative(options.Cache.LruCacheSizeKb, "LruCacheSizeKb")};");

        // Inverse mode: the ASP.NET Core middleware is the PUBLIC front door and
        // this nginx runs LOOPBACK-ONLY as an optimize-proxy behind it.
        var inverse = options.Sidecar.Mode == SidecarMode.Inverse;

        // ---- server scope ----
        sb.AppendLine("  server {");
        if (inverse)
        {
            // Loopback-only, no public listen, no TLS — the middleware owns the
            // public socket/TLS and forwards over loopback to this port (read from
            // InternalSidecarEndpoint, decoupled from the public Kestrel port).
            var nginxPort = _endpoint.NginxLoopbackPort;
            if (nginxPort == 0)
                throw new InvalidOperationException(
                    "Inverse mode: nginx loopback port is not set on InternalSidecarEndpoint; the raw-origin " +
                    "Kestrel configurator must run before the nginx config is generated.");
            sb.AppendLine($"    listen 127.0.0.1:{Port(nginxPort, "NginxLoopbackPort")};");
        }
        else
        {
            sb.AppendLine($"    listen {Port(options.Sidecar.ListenPort, "ListenPort")};");
        }
        sb.AppendLine("    server_name _;");
        sb.AppendLine($"    pagespeed {(options.Enabled ? "on" : "off")};");
        if (inverse)
        {
            // Honor the single validated X-Forwarded-Proto the middleware injects so
            // the module roots rewritten asset URLs at the public scheme (the
            // loopback hop is plain http). RespectXForwardedProto is on the
            // SafeCustomOptionKeys allowlist. ps_apply_x_forwarded_proto reads the
            // FIRST XFP, and the transformer guarantees exactly one validated value.
            sb.AppendLine("    pagespeed RespectXForwardedProto on;");
        }

        // RewriteLevel + filters (validated).
        sb.AppendLine($"    pagespeed RewriteLevel {RewriteLevel(options.RewriteLevel)};");
        if (!string.IsNullOrWhiteSpace(options.EnabledFilters))
            sb.AppendLine($"    pagespeed EnableFilters {FilterList(options.EnabledFilters!)};");
        if (!string.IsNullOrWhiteSpace(options.DisabledFilters))
            sb.AppendLine($"    pagespeed DisableFilters {FilterList(options.DisabledFilters!)};");

        // Authorized domains (the classic config-injection vector -> validated).
        // Distinct: ASP.NET config-binding APPENDS to a non-empty default List, so
        // appsettings repeating a default would otherwise emit Domain twice (nginx
        // then warns "AddDomain ... already in map"). Also de-dups operator typos.
        foreach (var d in options.Domains.AuthorizedDomains.Distinct(StringComparer.Ordinal))
            sb.AppendLine($"    pagespeed Domain {Domain(d)};");

        // CustomOptions raw passthrough — DEFAULT-DENY: the key must be a curated
        // safe tuning directive (never an admin/path/fetch/script directive), the
        // value is injection-checked. D2/D8.
        foreach (var (k, v) in options.CustomOptions)
            sb.AppendLine($"    pagespeed {SafeDirectiveKey(k)} {Value(v)};");

        // Admin ACL gate (owner decision A / §10): operator-configurable allow list
        // (default loopback only) + deny all + the bearer map + an optional limit_req.
        // Emitted as CASE-INSENSITIVE regex locations ordered BEFORE the .pagespeed.
        // resource regex below, because the module's admin dispatcher (ngx_pagespeed.cc
        // ps_route_admin_url -> StringCaseStartsWith) routes admin/stats/console
        // case-INSENSITIVELY in the precontent phase regardless of which location wins.
        // A case-sensitive prefix (^~ /pagespeed_admin) would let GET /PAGESPEED_ADMIN
        // fall through to `location /` yet still reach the admin handler, bypassing this
        // ACL layer. `~*` matches the module's own case-folding, and
        // first-regex-wins ordering keeps the .pagespeed. resource regex (and any
        // ExcludePaths regex) from shadowing the gate. Both layers apply; the IP ACL
        // holds even if the bearer is disabled. The module-level StrictAdminAccess
        // (real-TCP-peer loopback) is the authoritative backstop regardless of which
        // nginx location is selected.
        foreach (var (_, path) in AdminHandlers)
        {
            sb.AppendLine($"    location ~* \"^{path}\" {{");
            foreach (var allow in AllowDirectives(options.AdminAuth.AllowedIps))
                sb.AppendLine($"      {allow}");
            sb.AppendLine("      deny all;");
            if (options.AdminAuth.Enabled && options.AdminAuth.RateLimitRpm > 0)
                sb.AppendLine("      limit_req zone=pagespeed_admin burst=10 nodelay;");
            if (options.AdminAuth.Enabled)
                sb.AppendLine("      if ($admin_ok = 0) { return 403; }");
            sb.AppendLine($"      proxy_pass {proxyTarget};");
            sb.AppendLine("    }");
        }

        // The three REQUIRED handler locations (owner decision B) — without these
        // the optimizer 404s its own .pagespeed./static/beacon assets. Regexes are
        // VERBATIM from install/ngxpagespeed-com.conf; the {10} hash-segment length
        // is the .pagespeed. URL contract and MUST stay byte-identical. These come
        // AFTER the admin regexes so they can't shadow the admin gate.
        sb.AppendLine("    location ~ \"\\.pagespeed\\.([a-z]\\.)?[a-z]{2}\\.[^.]{10}\\.[^.]+\" { add_header \"\" \"\"; }");
        sb.AppendLine("    location ~ \"^/pagespeed_static/\" { }");
        sb.AppendLine("    location ~ \"^/ngx_pagespeed_beacon$\" { }");

        // The GENERATED health stub the sidecar polls (D6) — NOT a stock route.
        sb.AppendLine("    location = /pagespeed/health { return 200 \"pagespeed-sidecar-ok\\n\"; }");

        // ExcludePaths: paths the operator does not want optimized. Emitted as
        // regex locations with `pagespeed off;` ahead of the catch-all so they
        // still reverse-proxy but skip optimization. Each pattern is validated
        // fail-closed (injection class #4).
        foreach (var pattern in options.ExcludePaths)
        {
            sb.AppendLine($"    location ~ \"{LocationRegex(pattern)}\" {{");
            sb.AppendLine("      pagespeed off;");
            sb.AppendLine($"      proxy_pass {proxyTarget};");
            sb.AppendLine("      proxy_set_header Host $host;");
            sb.AppendLine("      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;");
            AppendInverseProxyHeaders(sb, inverse);
            sb.AppendLine("    }");
        }

        // Reverse proxy to the Kestrel origin. Loopback/UDS only (D8) — the target
        // is owned by InternalSidecarEndpoint so Kestrel + nginx never disagree. In
        // Inverse the target is the PRIVATE raw-origin loopback-TCP endpoint where
        // the middleware bypasses itself (the loop break); the IP-ACL on the admin
        // locations above no longer distinguishes peers (every connection to this
        // nginx is the loopback middleware), so the bearer is load-bearing and the
        // middleware 404s admin from the public front door unless AllowPublicAdmin.
        sb.AppendLine("    location / {");
        sb.AppendLine($"      proxy_pass {proxyTarget};");
        sb.AppendLine("      proxy_set_header Host $host;");
        sb.AppendLine("      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;");
        AppendInverseProxyHeaders(sb, inverse);
        sb.AppendLine("    }");

        sb.AppendLine("  }");
        sb.AppendLine("}");

        return (sb.ToString(), adminToken);
    }

    /// <summary>
    /// In Inverse mode, appends the two nginx-&gt;raw-origin proxy headers the
    /// front-proxy path does not need: forward the single validated
    /// X-Forwarded-Proto the middleware injected (so the raw-origin app and the
    /// module agree on the public scheme), and force Accept-Encoding to identity
    /// so the raw origin cannot pre-compress past any host ResponseCompression
    /// (nginx controls THAT request — the safer enforcement point; review fix).
    /// No-op in Process/External.
    /// </summary>
    private static void AppendInverseProxyHeaders(StringBuilder sb, bool inverse)
    {
        if (!inverse) return;
        sb.AppendLine("      proxy_set_header X-Forwarded-Proto $http_x_forwarded_proto;");
        sb.AppendLine("      proxy_set_header Accept-Encoding \"\";");
    }

    // ----------------------------------------------------------------------
    // Path / value resolution
    // ----------------------------------------------------------------------

    internal static string ResolveModulePath(PageSpeedOptions o, string? resolvedBinaryPath = null)
    {
        if (!string.IsNullOrEmpty(o.Sidecar.ModulePath))
            return Path.GetFullPath(o.Sidecar.ModulePath!);
        // Bundled layout (D5): the .so sits next to the nginx binary, both under the
        // app's runtimes/{rid}/native/. Derive the module dir from the ACTUAL resolved
        // binary first, so a binary found in the runtimes subdir and a module defaulted
        // to the AppContext.BaseDirectory root can't diverge into a `load_module` at a
        // path that doesn't exist (PKG-3). Fall back to an explicitly configured
        // BinaryPath, then AppContext.BaseDirectory.
        var baseDir =
            !string.IsNullOrEmpty(resolvedBinaryPath) ? Path.GetDirectoryName(Path.GetFullPath(resolvedBinaryPath!))!
            : !string.IsNullOrEmpty(o.Sidecar.BinaryPath) ? Path.GetDirectoryName(Path.GetFullPath(o.Sidecar.BinaryPath!))!
            : AppContext.BaseDirectory;
        return Path.Combine(baseDir, "ngx_pagespeed_module.so");
    }

    /// <summary>
    /// Logs a clear warning for bound-but-unmapped option surfaces. v1 emits the core
    /// directive table and routes extra tuning through
    /// <see cref="PageSpeedOptions.CustomOptions"/>; <see cref="PageSpeedOptions.Redis"/>
    /// and the domain rewrite/origin/shard mappings are NOT yet emitted by the nginx
    /// generator in this preview, so warn rather than silently dropping operator config
    /// (a silent-trap robustness gap). the design record §scope: dropped surfaces get a note.
    /// </summary>
    private void WarnUnmappedOptions(PageSpeedOptions o)
    {
        if (o.Redis is not null && !string.IsNullOrWhiteSpace(o.Redis.Host))
            _logger.LogWarning(
                "PageSpeed:Redis is configured but the nginx sidecar does not emit a RedisServer directive in " +
                "this preview; the file cache is used instead.");
        if (o.Domains.RewriteMappings.Count > 0 || o.Domains.OriginMappings.Count > 0 || o.Domains.Shards.Count > 0)
            _logger.LogWarning(
                "PageSpeed:Domains rewrite/origin/shard mappings are configured but not emitted by the nginx sidecar " +
                "in this preview; they are ignored.");
    }

    private static string ResolveCacheDir(PageSpeedOptions o, string prefix) =>
        Path.GetFullPath(o.Cache.FileCachePath ?? Path.Combine(prefix, "cache"));

    private static string ResolveLogDir(PageSpeedOptions o, string prefix) =>
        Path.GetFullPath(o.Cache.LogDirectory ?? Path.Combine(prefix, "logs"));

    // ----------------------------------------------------------------------
    // Fail-closed typed validation (the D8 attack surface)
    // ----------------------------------------------------------------------

    // Whole comma-list of filters, each token optionally +/- prefixed. The charset
    // alone is the security control (no whitespace/quote/metachar can break out);
    // unknown filter names are a no-op/warning at config load, not an injection.
    private static readonly Regex FilterRe = new("^[-+a-z0-9_,]+$", RegexOptions.Compiled);

    // Hostname (RFC-1123 labels, incl. single-label like 'localhost' and bare
    // IPv4), optional leading '*.' wildcard, optional http(s):// scheme, optional
    // :port. Excludes paths/userinfo/'@'/space and every nginx/shell metachar.
    private static readonly Regex DomainRe = new(
        @"^(https?://)?(\*\.)?([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*(:[0-9]{1,5})?$",
        RegexOptions.Compiled);

    private static readonly Regex KeyRe = new("^[A-Za-z][A-Za-z0-9]*$", RegexOptions.Compiled);

    // RewriteLevel: case-insensitive match against the six canonical spellings
    // (RewriteOptions::ParseRewriteLevel); emit the canonical casing.
    private static readonly string[] RewriteLevels =
        { "PassThrough", "CoreFilters", "OptimizeForBandwidth", "MobilizeFilters", "TestingCoreFilters", "AllFilters" };

    // Default-deny allowlist of CustomOptions directives that are safe to expose
    // to customer config — scalar tuning knobs only (inlining thresholds,
    // compression quality, parallel-rewrite caps, cache TTLs, boolean flags). A
    // reviewed security artifact pinned to the bundled mod_pagespeed source
    //. Compared ordinally (directive names are PascalCase).
    private static readonly HashSet<string> SafeCustomOptionKeys = new(StringComparer.Ordinal)
    {
        "CssInlineMaxBytes", "CssFlattenMaxBytes", "CssImageInlineMaxBytes", "CssOutlineMinBytes",
        "GoogleFontCssInlineMaxBytes", "ImageInlineMaxBytes", "ImageMaxRewritesAtOnce",
        "JsInlineMaxBytes", "JsOutlineMinBytes", "ImageRecompressionQuality", "JpegRecompressionQuality",
        "JpegRecompressionQualityForSmallScreens", "JpegQualityForSaveData", "WebpRecompressionQuality",
        "WebpRecompressionQualityForSmallScreens", "WebpAnimatedRecompressionQuality", "WebpQualityForSaveData",
        "ImageJpegNumProgressiveScans", "ImageJpegNumProgressiveScansForSmallScreens", "MaxCombinedCssBytes",
        "MaxCombinedJsBytes", "MaxHtmlParseBytes", "MaxImageSizeLowResolutionBytes", "ImageResolutionLimitBytes",
        "ImageLimitOptimizedPercent", "ImageLimitResizeAreaPercent", "ImageLimitRenderedAreaPercent",
        "ProgressiveJpegMinBytes", "ModifyCachingHeaders", "RespectVary", "FlushHtml", "ImplicitCacheTtlMs",
        "OverrideCachingTtlMs", "MinResourceCacheTimeToRewriteMs", "MaxCacheableContentLength", "MaxHtmlCacheTimeMs",
        "MaxSegmentLength", "MaxUrlSize", "WebpTimeoutMs", "FlushBufferLimitBytes", "IdleFlushTimeMs",
        "DomainShardCount", "RewriteRandomDropPercentage", "BeaconReinstrumentTimeSec", "ExperimentCookieDurationMs",
        "OptionCookiesDurationMs", "InPlaceRewriteDeadlineMs", "InPlaceSMaxAgeSec", "RespectXForwardedProto",
        "HonorCsp", "StickyQueryParameters",
    };

    // nginx config-injection breakout set: directive/block terminators, quotes,
    // the backslash escape, the variable sigil, and every whitespace/control char
    // nginx could treat as a token separator or that could smuggle a newline past
    // a log/visual review. The non-ASCII separators (NEL, LS, PS) and the rarer
    // ASCII controls (VT, FF) are not exploitable for config-injection (nginx
    // tokenizes on ASCII whitespace + ;{}#), but are rejected as defense-in-depth
    // so no exotic whitespace ever reaches the generated config.
    private static readonly char[] NginxMetacharacters =
        { ';', '{', '}', '#', '\n', '\r', '\0', '"', '\'', '\\', '$', ' ', '\t',
          '\v', '\f', '\u0085', '\u2028', '\u2029' };

    /// <summary>Hard-reject any value that could break out of its directive.</summary>
    private static void RejectInjection(string value, string field)
    {
        if (value is null) throw new ArgumentException($"{field} is null");
        if (value.IndexOfAny(NginxMetacharacters) >= 0)
            throw new InvalidOperationException(
                $"PageSpeed option '{field}' contains an nginx-config metacharacter; refusing to generate (fail-closed).");
    }

    private static string RewriteLevel(string v)
    {
        var match = RewriteLevels.FirstOrDefault(l => string.Equals(l, v, StringComparison.OrdinalIgnoreCase));
        if (match is null)
            throw new InvalidOperationException(
                $"Invalid RewriteLevel '{v}' (allowed: {string.Join(", ", RewriteLevels)}).");
        return match; // canonical casing
    }

    private static string FilterList(string v)
    {
        var compact = v.Replace(" ", "");
        if (!FilterRe.IsMatch(compact))
            throw new InvalidOperationException($"Invalid filter list '{v}' (allowed: [-+a-z0-9_,]).");
        return compact;
    }

    private static string Domain(string v)
    {
        RejectInjection(v, "Domain");
        if (!DomainRe.IsMatch(v))
            throw new InvalidOperationException($"Invalid Domain '{v}'.");
        return v;
    }

    /// <summary>
    /// Default-deny CustomOptions key: must be a curated safe tuning directive.
    /// Admin/path/fetch/script directives and anything not on the allowlist are
    /// rejected (fail-closed) so customer config can never reach a security-
    /// relevant directive via the passthrough.
    /// </summary>
    private static string SafeDirectiveKey(string k)
    {
        if (k is null || !KeyRe.IsMatch(k))
            throw new InvalidOperationException($"Invalid CustomOptions key '{k}' (must be a PascalCase directive name).");
        if (!SafeCustomOptionKeys.Contains(k))
            throw new InvalidOperationException(
                $"CustomOptions key '{k}' is not on the sidecar's allowlist of safe tuning directives (default-deny). " +
                "Admin/path/fetch/script directives cannot be set via CustomOptions.");
        return k;
    }

    private static string Value(string v)
    {
        RejectInjection(v, "CustomOptions value");
        return v;
    }

    private static int Port(int p, string field)
    {
        if (p is < 1 or > 65535)
            throw new InvalidOperationException($"{field} {p} out of range (1-65535).");
        return p;
    }

    private static long NonNegative(long n, string field)
    {
        if (n < 0) throw new InvalidOperationException($"{field} {n} must be >= 0.");
        return n;
    }

    /// <summary>
    /// Validates AdminAuth.RateLimitRpm before it is emitted into the
    /// <c>limit_req_zone ... rate=&lt;N&gt;r/m</c> directive. Callers gate on
    /// <c>&gt; 0</c> (0 disables), so this only guards the upper bound (a pathological
    /// value would build an absurd nginx zone). nginx requires a positive integer rate.
    /// </summary>
    private static int RateLimit(int rpm)
    {
        if (rpm is < 1 or > 1_000_000)
            throw new InvalidOperationException($"AdminAuth.RateLimitRpm {rpm} out of range (1-1000000).");
        return rpm;
    }

    /// <summary>
    /// Validates an admin token (operator-supplied or generated): base64/hex/url-
    /// safe charset only, so it can never break out of the quoted map key. Returns
    /// null for a null/empty input (caller generates one).
    /// </summary>
    private static string? TokenSafe(string? token, string field)
    {
        if (string.IsNullOrEmpty(token)) return null;
        // Bound the length so "Bearer " + token stays under the map_hash_bucket_size
        // budget (128) — a longer token makes nginx fail config-load with a precise
        // error instead of an opaque [emerg] at startup.
        if (token.Length > 100)
            throw new InvalidOperationException(
                $"{field} is too long (max 100 chars); a longer token would overflow nginx's map_hash_bucket_size and prevent the sidecar from starting.");
        if (!Regex.IsMatch(token, @"^[A-Za-z0-9._\-+/=]+$"))
            throw new InvalidOperationException(
                $"{field} contains characters outside the safe token charset [A-Za-z0-9._-+/=].");
        return token;
    }

    /// <summary>
    /// Validates each AdminAuth.AllowedIps entry as a bare IP or CIDR and returns
    /// the nginx <c>allow &lt;entry&gt;;</c> directives. Fail-closed on anything that
    /// is not a parseable IP/prefix (no injection can reach the config).
    /// </summary>
    private static IEnumerable<string> AllowDirectives(IEnumerable<string> allowedIps)
    {
        // Distinct for the same config-binding-append reason as AuthorizedDomains.
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var raw in allowedIps)
        {
            var entry = raw.Trim();
            RejectInjection(entry, "AllowedIps");
            var slash = entry.IndexOf('/');
            var addr = slash >= 0 ? entry[..slash] : entry;
            var prefix = slash >= 0 ? entry[(slash + 1)..] : null;

            if (!IPAddress.TryParse(addr, out var ip))
                throw new InvalidOperationException($"Invalid AllowedIps entry '{raw}' (not an IP/CIDR).");
            if (prefix is not null)
            {
                var max = ip.AddressFamily == AddressFamily.InterNetworkV6 ? 128 : 32;
                if (!int.TryParse(prefix, out var bits) || bits < 0 || bits > max)
                    throw new InvalidOperationException($"Invalid AllowedIps prefix in '{raw}' (0-{max}).");
            }

            if (seen.Add(entry))
                yield return $"allow {entry};";
        }
    }

    /// <summary>
    /// Validates an ExcludePaths nginx location regex: rejects the quote/metachar
    /// set that could terminate the quoted pattern or inject a directive/block,
    /// while permitting regex syntax (^ $ . * + ? ( ) [ ] | / \ -). Fail-closed.
    /// </summary>
    private static string LocationRegex(string pattern)
    {
        if (string.IsNullOrWhiteSpace(pattern))
            throw new InvalidOperationException("ExcludePaths pattern is empty.");
        // Same breakout set as NginxMetacharacters minus the regex-syntax chars this
        // field legitimately uses ($ \ are allowed here). The exotic-whitespace
        // controls (VT/FF/NEL/LS/PS) are rejected as defense-in-depth.
        if (pattern.IndexOfAny(
                new[] { '"', ';', '{', '}', '#', '\n', '\r', '\0', ' ', '\t',
                        '\v', '\f', '\u0085', '\u2028', '\u2029' }) >= 0)
            throw new InvalidOperationException(
                $"ExcludePaths pattern '{pattern}' contains an nginx-config metacharacter; refusing to generate (fail-closed).");
        // A trailing ODD run of backslashes would escape the emitted closing quote
        // ( ...\" ), breaking out of the quoted location string. (Quotes themselves
        // are already rejected above, so this is the only backslash-escape vector.)
        var trailing = pattern.Length - pattern.TrimEnd('\\').Length;
        if (trailing % 2 != 0)
            throw new InvalidOperationException(
                $"ExcludePaths pattern '{pattern}' ends in an unbalanced backslash; refusing to generate (fail-closed).");
        return pattern;
    }

    // ----------------------------------------------------------------------
    // helpers
    // ----------------------------------------------------------------------

    private static string GenerateRandomToken()
    {
        var bytes = new byte[32];
        RandomNumberGenerator.Fill(bytes);
        return Convert.ToHexString(bytes).ToLowerInvariant();
    }

    private static void TrySetMode(string path, UnixFileMode mode)
    {
        if (OperatingSystem.IsWindows()) return;
        try { File.SetUnixFileMode(path, mode); } catch { /* best-effort */ }
    }
}

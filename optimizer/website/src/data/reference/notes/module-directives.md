# Notes for the native module directives

Operator prose appended to the generated blocks on `/docs/configuration/`.
One `## <directive>` section per directive; the name must exist in
`module-directives.json` or rendering fails. Syntax, default and context are
generated from the source and must not be repeated here.

## RewriteLevel

The baseline filter set. `CoreFilters` is the production default most sites
start from; `PassThrough` runs only the filters you enable by name;
`OptimizeForBandwidth` optimizes resources in place without rewriting URLs;
`AllFilters` switches on every filter outside the dangerous set, including the
opt-in AVIF filters. `MobilizeFilters` and `TestingCoreFilters` exist for the
project's own testing. See [Filter selection](/docs/filter-selection/#rewritelevel).

## EnableFilters

The filter names are on the [filters reference](/docs/filters/). See
[Filter selection](/docs/filter-selection/#enablefilters) for scoping and
interaction with `RewriteLevel`.

## DisableFilters

See [Filter selection](/docs/filter-selection/#disablefilters).

## Disallow

<a id="pagespeed_disallow"></a>

The thin nginx module's `pagespeed_disallow` is the equivalent for the Docker and
reverse-proxy shape; see
[Worker and reverse-proxy configuration](/docs/worker-configuration/#pagespeed_disallow).

## ForbidFilters

A forbidden filter cannot be re-enabled by a more specific scope or by a query
parameter. See [Filter selection](/docs/filter-selection/#forbidfilters).

## ForbidAllDisabledFilters

See [Filter selection](/docs/filter-selection/#forbidalldisabledfilters).

## HonorCsp

Enabled by default. The module reads `Content-Security-Policy` response headers
and meta tags and suppresses optimizations the policy would block, such as
inlining resources or injecting scripts. Set it to `off` to optimize without
consulting CSP, and verify pages that rely on strict policies. Since
v1.15.0+r18 the filters that inject inline scripts also back off cleanly when
the policy disallows inline script.

## RespectVary

When enabled, resources whose `Vary` header indicates per-request variation are
not rewritten.

## DisableRewriteOnNoTransform

When enabled, resources served with `Cache-Control: no-transform` are left
alone.

## LowercaseHtmlNames

When enabled, HTML tag and attribute names are lowercased during parsing.

## ModifyCachingHeaders

Controls whether the module sets caching headers on HTML responses. Do not
disable it unless you fully understand the interaction with downstream caches:
disabling it can cause stale optimized content to be served.

## XHeaderValue

Sets the value of the `X-Mod-Pagespeed` (Apache) or `X-Page-Speed` (nginx, IIS)
[response header](/pagespeed-markers/#x-page-speed). From 2.7.0 the default is the generic
value `on`, which does not disclose the installed version. A value set here
takes precedence over the default and over
[`ExposeVersionInHeaders`](#expose-version-in-headers), the server-wide option that
restores the module version string.

## PreserveUrlRelativity

When enabled, relative URLs in rewritten HTML stay relative instead of being
converted to absolute URLs.

## StaticAssetPrefix

URL prefix the module serves its own static assets from (the JavaScript
libraries and images some filters inject). The default is `/pagespeed_static/`.

## AddResourceHeader

Repeatable: specify it once per header, up to 20. Since v1.15.0+r18 the limit is
enforced exactly.

From 2.7.0, a configured header replaces the origin's header of the same name: if the origin
already sends `Access-Control-Allow-Origin` and you configure that name, the
response carries only your value. Several directives with the same name are all
sent, one header line each. In 2.6.0 and earlier your value is added after the
origin's, so a response can carry both.

```apache
ModPagespeedAddResourceHeader "X-Custom" "value"
```

```nginx
pagespeed AddResourceHeader "X-Custom" "value";
```

## CssFlattenMaxBytes

Set it in the server or directory configuration. From 2.8.0 it can no longer
be set on a single request: a `PageSpeedCssFlattenMaxBytes` query parameter,
request or response header, or cookie has no effect. In 2.7.0 and earlier it
could also be set per request.

## ListOutstandingUrlsOnError

Enable it only while debugging fetch problems; it is noisy in production.

## NumRewriteThreads

Global: set it once per server, not per virtual host or location. How `auto`
resolves per server, and when to override it, is in
[Optimization threads](#optimization-threads).

## NumExpensiveRewriteThreads

Global, like `NumRewriteThreads`. See [Optimization threads](#optimization-threads).

## MaxSegmentLength

Why and when to lower it is in [URL segment length limits](#max-url-segments).

## DaemonSocketPath

Set together with `DaemonVolumePath` to hand in-place optimization to the
optimizer worker installed beside the module. The worker's own flags are on
[Worker and reverse-proxy configuration](/docs/worker-configuration/).

## DaemonVolumePath

Keep this on a different path from `FileCachePath`. The worker creates the
volume under its `--cache-dir`; see
[Worker and reverse-proxy configuration](/docs/worker-configuration/#--cache-dir).

## UseExperimentalJsMinifier

Deprecated since v1.15.0+r21: the tokenizer-based minifier is the only
JavaScript minifier. The directive is accepted for compatibility, ignored, and
logs a deprecation warning at configuration load. Remove it from your
configuration.

## UseNativeFetcher

Settable in the `http` block only. The native fetcher handles HTTP and HTTPS
fetches.

## WebBotAuthBotDetection

Opt-in: a cryptographically verified Web Bot Auth signature (RFC 9421)
classifies the request as automated for the module's own bot detection, so a
signed agent is recognized even when it presents a browser user agent. Requires
`WebBotAuth`. See [Web Bot Auth](/docs/web-bot-auth/).

## RslCapEnforcement

See [RSL-CAP enforcement](/docs/rsl-cap/).

## RulePackFile

Repeatable: each line adds a pack file, up to eight per server or virtual host
(a ninth is a configuration error, and two lines naming the same path count
once). The packs run in the order of the lines. A virtual host that sets
`RulePackFile` uses its own list instead of the server-wide one. See
[Rule packs](/docs/rule-packs/#enable).

## AutoCreateCachePath

Creation applies only to a cache directory inside
`C:\ProgramData\We-Amp\PageSpeed\cache\` or
`C:\ProgramData\We-Amp\IISWebSpeed\cache\`; a cache directory anywhere else
must already exist when the site serves its first request, whatever this is set
to. With `off`, a missing directory is not created and the site does not start
optimizing. See
[IIS configuration](/docs/iis-configuration/#automatic-cache-directory-creation).

## InfoUrlsLocalOnly

A request is local when it comes from the server itself and carries no
reverse-proxy forwarding headers; `TreatProxiedRequestsAsLocal` changes that
for one site. See [Admin console access control](/docs/admin-console/#access-control).

## TreatProxiedRequestsAsLocal

Turning it on serves these pages to anyone who reaches the site through that
proxy or tunnel; prefer the `*Domains` allowlists.

---
title: 'Configuration reference: Apache and nginx directives'
seoTitle: 'mod_pagespeed configuration: Apache, nginx, IIS directives'
description: 'Every mod_pagespeed directive for Apache, nginx and IIS, generated from the module source: syntax, default and context, and where the config file lives.'
order: 20
group: 'Configure'
lastUpdated: 2026-11-14
faq:
  - q: 'Where is the mod_pagespeed configuration file?'
    a: 'Apache reads `pagespeed.conf` from `/etc/apache2/mods-available/` (Debian, Ubuntu) or `/etc/httpd/conf.d/` (RHEL, AlmaLinux, Rocky); nginx reads `pagespeed` directives from `nginx.conf`, in the `http`, `server` or `location` block; IIS reads `pagespeed.config` from the site root or `%ProgramData%\We-Amp\PageSpeed\`.'
  - q: 'What is the minimal working configuration?'
    a: 'On Apache, `ModPagespeed on` and a writable `ModPagespeedFileCachePath`; the packaged `pagespeed.conf` already sets both. On nginx, the package already loads the module; add `pagespeed on;` and `pagespeed FileCachePath /var/cache/ngx_pagespeed;` in the server block, plus the three location blocks that let the module serve its own resources. On IIS, `pagespeed on` in `pagespeed.config`.'
  - q: 'How do I write the same directive for Apache, nginx and IIS?'
    a: 'Prefix the directive name with `ModPagespeed` on Apache (`ModPagespeedEnableFilters rewrite_images`); on nginx write `pagespeed` followed by the name and a semicolon (`pagespeed EnableFilters rewrite_images;`); on IIS use the nginx form without the semicolon in `pagespeed.config`.'
  - q: 'How do I turn mod_pagespeed off for one path?'
    a: 'Scope the switch: `ModPagespeed off` inside a `<Location>` or `<Directory>` block on Apache, `pagespeed off;` inside a `location` block on nginx, or a `Disallow` directive for a URL wildcard on any server.'
---

:::tip[Running the Docker image or the worker?]
This page is the reference for the **native module**: the in-process
`mod_pagespeed` for Apache and nginx (and the IIS module, which reads the same
directives). If you run the `pagespeed-nginx` image, the Helm chart or the
optimizer worker beside a thin nginx, the 16 `pagespeed_*` directives and the
worker's command-line flags are on
[Worker and reverse-proxy configuration](/docs/worker-configuration/).
:::

mod_pagespeed is configured with directives in the web server's own
configuration. This page lists every one of them, generated from the module's
source so the list is the list the shipped module accepts: for each directive
the syntax on Apache and nginx, the default, the contexts it may appear in and
the module's own description, grouped by area. It starts with where the
configuration lives and the smallest configuration that works.

## Where the configuration lives

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

nginx has no separate file: `pagespeed` directives go into `nginx.conf` (or a
file it includes) in the `http`, `server` or `location` block. The package
auto-enables the module's `load_module` line on a stock nginx; every directive
is written as `pagespeed Name value;`.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

The package drops `pagespeed.conf` into `/etc/apache2/mods-available/` (Debian,
Ubuntu) or `/etc/httpd/conf.d/` (RHEL, AlmaLinux, Rocky) and loads the module.
Edit that file, or put directives in a virtual host, a `<Directory>` or
`<Location>` block, or `.htaccess`; every directive is written as
`ModPagespeedName value`.

On cPanel / EasyApache 4 the file is `/etc/apache2/conf.d/pagespeed.conf`, and
the module cannot run beside `mod_ruid2`, which switches each request to the
site owner's uid while the module's cache belongs to the Apache user: remove
mod_ruid2 first, as [the cPanel page](/docs/cpanel/#mod-ruid2) explains.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

The installer registers the module and reads `pagespeed.config`, a flat text
file with one directive per line, from the website root (site-level) and from
`%ProgramData%\We-Amp\PageSpeed\pagespeed.config` (server-level); site-level
settings override server-level ones. If `pagespeed.config` is not found the
module falls back to `iiswebspeed.config`, the filename of IISpeed installs.
Directives are written as `pagespeed Name value` without a semicolon; the
`ModPagespeed` and `iispeed` prefixes are accepted for compatibility. See
[IIS configuration](/docs/iis-configuration/) for match rules and the file
format.

</div>

## Minimal working configuration

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
# The package loads the module; check with:
#   sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx /usr/share/nginx/modules
# Only a module installed by hand needs this line at the top of nginx.conf:
# load_module modules/ngx_pagespeed_module.so;                    # Debian / Ubuntu
# load_module /usr/lib64/nginx/modules/ngx_pagespeed_module.so;   # Enterprise Linux

http {
    server {
        pagespeed on;
        pagespeed FileCachePath /var/cache/ngx_pagespeed;

        # Let the module serve its own resources.
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
        }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon$" { }
    }
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeed on
ModPagespeedFileCachePath /var/cache/mod_pagespeed/
```

The packaged `pagespeed.conf` already contains both, so a fresh install
optimizes with `RewriteLevel CoreFilters` without any edit.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed on
```

</div>

With that in place the module runs the CoreFilters set. Everything below is
optional: pick a different [rewrite level](#rewritelevel), add or remove
[filters](/docs/filters/), and tune the directives of the area you care about.
To see which filters your own pages would benefit from, run them through a
[PageSpeed Insights test](/analyze/).

## How to read the reference

- **Syntax** shows the Apache spelling (`ModPagespeedName …`) and the nginx
  spelling (`pagespeed Name …;`). IIS uses the nginx spelling without the
  semicolon. Placeholders: `on|off` is a boolean (`true`/`false` also work),
  `number` an integer, `value` a string, `wildcard` a URL pattern with `*`.
- **Default** is the value compiled into the module; `(empty)` means unset, and
  `—` that the directive has no value of its own.
- **Context** lists where the directive may appear. On Apache: the server
  config, a `<VirtualHost>`, a `<Directory>`/`<Location>` block, `.htaccess`.
  On nginx: the `http`, `server` and `location` blocks. On IIS:
  `pagespeed.config`, and whether the directive may also appear under a match
  rule. "Also per request"
  marks the options that can also be set on a single request with a
  `PageSpeed…` query parameter or header.
- **Platform** appears when a directive exists on one server only. An option
  whose block says the source carries no help text is accepted by nginx and
  IIS but is not registered as an Apache directive.
- A **since** version is not shown: the module source does not record the
  release a directive first appeared in. Additions are listed per release in
  the [release notes](/docs/release-notes/).

The nginx versions each shape runs on are in one table on the
[worker and reverse-proxy page](/docs/worker-configuration/#which-nginx-version).

## Module states

The module switch, [`pagespeed` / `ModPagespeed`](#modpagespeed), takes three
states:

| State       | Behavior                                                                                                                                                                                                  |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `on`        | Full optimization. The module rewrites HTML and serves optimized resources.                                                                                                                               |
| `standby`   | Serves previously optimized `.pagespeed.` resources and responds to query-parameter requests, but does not optimize new traffic. Use this to drain optimized resources before fully disabling the module. |
| `unplugged` | Fully disabled. The module does not intercept any requests. This state can only be set at the top level or within a virtual host block, not in directory-level configuration.                             |

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed standby;
```

On nginx, `pagespeed off;` is an alias for `unplugged`.

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeed standby
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed standby
```

</div>

## Scoping directives {#location-specific-configuration}

Directives can be scoped to parts of a site; a value set at a higher level is
inherited by the blocks inside it. The scoping mechanisms differ per server.

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

Apache supports configuration at several levels:

- **`pagespeed.conf`** — Global defaults that apply to all virtual hosts.
- **`<VirtualHost>`** — Per-site overrides.
- **`<Directory>` and `<Location>`** — Target specific filesystem paths or URL paths.
- **`.htaccess`** — Per-directory configuration placed in the document root or subdirectories.

`.htaccess` configuration is re-read on every request, which adds per-request
overhead. For high-traffic sites, prefer `<Directory>` or `<Location>` blocks in
the server configuration.

```apache
<Location /images/>
    ModPagespeedDisableFilters convert_jpeg_to_webp
</Location>
```

</div>

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

In nginx, use `server` and `location` blocks to scope directives:

```nginx
server {
    pagespeed on;

    location /static/ {
        pagespeed off;
    }
}
```

Directives set in a `server` block apply to all locations within that server
unless overridden by a more specific `location` block. Directives whose context
is `http` only are global and are rejected elsewhere.

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

See [IIS configuration](/docs/iis-configuration/#path-based-matching-with-regex)
for IIS path- and hostname-based scoping.

</div>

## Virtual hosts

Each virtual host can carry its own mod_pagespeed configuration. Directives set
at the global level serve as defaults; virtual host configuration overrides
them.

One requirement: HTML pages and the optimized resources they reference must
share the same configuration options. If a virtual host serves HTML that
references resources optimized under a different set of options, the resources
may not be found or may be re-optimized unnecessarily.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
server {
    server_name site-a.example.com;
    pagespeed on;
    pagespeed EnableFilters rewrite_images;
}

server {
    server_name site-b.example.com;
    pagespeed on;
    pagespeed EnableFilters collapse_whitespace;
}
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
<VirtualHost *:80>
    ServerName site-a.example.com
    ModPagespeed on
    ModPagespeedEnableFilters rewrite_images
</VirtualHost>

<VirtualHost *:80>
    ServerName site-b.example.com
    ModPagespeed on
    ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

Place a `pagespeed.config` file in each website's root directory. Each file can
carry its own set of directives:

**Site A** (`C:\inetpub\site-a\pagespeed.config`):

```text
pagespeed on
pagespeed EnableFilters rewrite_images
```

**Site B** (`C:\inetpub\site-b\pagespeed.config`):

```text
pagespeed on
pagespeed EnableFilters collapse_whitespace
```

Alternatively, use match rules in the server-level config to scope directives
by hostname:

```text
hostname:^site-a\.example\.com$
pagespeed EnableFilters rewrite_images

hostname:^site-b\.example\.com$
pagespeed EnableFilters collapse_whitespace
```

</div>

<a id="configuration-directives"></a>

## Directive reference {#native-module-configuration-directives}

Every directive the module registers, generated from the source at the release
the [source pin](https://github.com/We-Amp/pagespeed-optimizer/blob/main/website/src/data/reference/source-pin.json)
names, grouped by area. Since v1.15.0+r18 configuration validation is strict:
out-of-range values for bounded options (image quality levels, progressive JPEG
scan counts, `HttpCacheCompressionLevel`, `RewriteRandomDropPercentage`) fail
configuration load instead of being silently accepted, an invalid filter name in
`?PageSpeedFilters=` rejects the whole query, and directive matching is
case-consistent. A configuration that loaded on an earlier revision may need its
values corrected when upgrading.

<!-- generated:begin module-directives -->

Generated from the mod_pagespeed source at tag [`v2.8.1`](https://github.com/We-Amp/mod_pagespeed/tree/v2.8.1): 338 directives.

Areas: [Enabling and filter selection](#area-enabling-and-filter-selection) (13) · [Request handling and policy](#area-request-handling-and-policy) (33) · [In-place resource optimization](#area-in-place-resource-optimization) (14) · [Domains and URLs](#area-domains-and-urls) (26) · [Optimizer worker](#area-optimizer-worker) (7) · [Rule packs](#area-rule-packs) (4) · [Bot authentication and licensing](#area-bot-authentication-and-licensing) (19) · [Statistics, console and logging](#area-statistics-console-and-logging) (33) · [Images](#area-images) (44) · [CSS](#area-css) (10) · [JavaScript](#area-javascript) (9) · [Caching](#area-caching) (49) · [Fetching and origins](#area-fetching-and-origins) (19) · [HTML rewriting and page hints](#area-html-rewriting-and-page-hints) (11) · [Threads and limits](#area-threads-and-limits) (4) · [Experiments and analytics](#area-experiments-and-analytics) (6) · [Other](#area-other) (2) · [Deprecated and ignored](#area-deprecated-and-ignored) (35).

### Enabling and filter selection {#area-enabling-and-filter-selection}

[`Allow`](#allow), [`DisableFilters`](#disablefilters), [`Disallow`](#disallow), [`EnableAggressiveRewritersForMobile`](#enableaggressiverewritersformobile), [`EnableFilters`](#enablefilters), [`EnableRewriting`](#enablerewriting), [`ForbidAllDisabledFilters`](#forbidalldisabledfilters), [`ForbidFilters`](#forbidfilters), [`ModPagespeed`](#modpagespeed), [`RewriteDeadlinePerFlushMs`](#rewritedeadlineperflushms), [`RewriteLevel`](#rewritelevel), [`RewriteRandomDropPercentage`](#rewriterandomdroppercentage), [`RewriteUncacheableResources`](#rewriteuncacheableresources)

#### Allow {#allow}

- **Syntax:** `ModPagespeedAllow wildcard` (Apache) · `pagespeed Allow wildcard;` (nginx) · `pagespeed Allow wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allows optimization of resources whose URL matches the wildcard. Later Allow and Disallow directives override earlier ones.

#### DisableFilters {#disablefilters}

- **Syntax:** `ModPagespeedDisableFilters filter[,filter...]` (Apache) · `pagespeed DisableFilters filter[,filter...];` (nginx) · `pagespeed DisableFilters filter[,filter...]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Comma-separated list of disabled filters

See [Filter selection](/docs/filter-selection/#disablefilters).

#### Disallow {#disallow}

- **Syntax:** `ModPagespeedDisallow wildcard` (Apache) · `pagespeed Disallow wildcard;` (nginx) · `pagespeed Disallow wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Excludes resources (and pages) whose URL matches the wildcard from optimization.

<a id="pagespeed_disallow"></a>

The thin nginx module's `pagespeed_disallow` is the equivalent for the Docker and
reverse-proxy shape; see
[Worker and reverse-proxy configuration](/docs/worker-configuration/#pagespeed_disallow).

#### EnableAggressiveRewritersForMobile {#enableaggressiverewritersformobile}

- **Syntax:** `ModPagespeedEnableAggressiveRewritersForMobile on|off` (Apache) · `pagespeed EnableAggressiveRewritersForMobile on|off;` (nginx) · `pagespeed EnableAggressiveRewritersForMobile on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allows defer_javascript and defer_iframe for mobile browsers

#### EnableFilters {#enablefilters}

- **Syntax:** `ModPagespeedEnableFilters filter[,filter...]` (Apache) · `pagespeed EnableFilters filter[,filter...];` (nginx) · `pagespeed EnableFilters filter[,filter...]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Comma-separated list of enabled filters

The filter names are on the [filters reference](/docs/filters/). See
[Filter selection](/docs/filter-selection/#enablefilters) for scoping and
interaction with `RewriteLevel`.

#### EnableRewriting {#enablerewriting}

- **Syntax:** `pagespeed EnableRewriting on|off|unplugged|standby;` (nginx) · `pagespeed EnableRewriting on|off|unplugged|standby` (IIS)
- **Default:** `on`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ForbidAllDisabledFilters {#forbidalldisabledfilters}

- **Syntax:** `ModPagespeedForbidAllDisabledFilters on|off` (Apache) · `pagespeed ForbidAllDisabledFilters on|off;` (nginx) · `pagespeed ForbidAllDisabledFilters on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Prevents the use of disabled filters

See [Filter selection](/docs/filter-selection/#forbidalldisabledfilters).

#### ForbidFilters {#forbidfilters}

- **Syntax:** `ModPagespeedForbidFilters filter[,filter...]` (Apache) · `pagespeed ForbidFilters filter[,filter...];` (nginx) · `pagespeed ForbidFilters filter[,filter...]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Comma-separated list of forbidden filters

A forbidden filter cannot be re-enabled by a more specific scope or by a query
parameter. See [Filter selection](/docs/filter-selection/#forbidfilters).

#### pagespeed / ModPagespeed {#modpagespeed}

- **Syntax:** `ModPagespeed on|off|unplugged|standby` (Apache) · `pagespeed on|off|unplugged|standby;` (nginx) · `pagespeed on|off|unplugged|standby` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

The module switch. on optimizes; standby serves already-optimized .pagespeed. resources and answers query-parameter requests but optimizes no new traffic; unplugged intercepts nothing (on nginx, off is an alias for unplugged). unplugged can only be set at the top level or in a virtual host. nginx needs an explicit pagespeed on; in the server block.

#### RewriteDeadlinePerFlushMs {#rewritedeadlineperflushms}

- **Syntax:** `ModPagespeedRewriteDeadlinePerFlushMs number` (Apache) · `pagespeed RewriteDeadlinePerFlushMs number;` (nginx) · `pagespeed RewriteDeadlinePerFlushMs number` (IIS)
- **Default:** `10`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Time to wait for resource optimization (per flush window) beforefalling back to the original resource for the request.

#### RewriteLevel {#rewritelevel}

- **Syntax:** `ModPagespeedRewriteLevel CoreFilters|PassThrough|OptimizeForBandwidth|MobilizeFilters|TestingCoreFilters|AllFilters` (Apache) · `pagespeed RewriteLevel CoreFilters|PassThrough|OptimizeForBandwidth|MobilizeFilters|TestingCoreFilters|AllFilters;` (nginx) · `pagespeed RewriteLevel CoreFilters|PassThrough|OptimizeForBandwidth|MobilizeFilters|TestingCoreFilters|AllFilters` (IIS)
- **Default:** `CoreFilters`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Base level of rewriting (PassThrough, CoreFilters)

The baseline filter set. `CoreFilters` is the production default most sites
start from; `PassThrough` runs only the filters you enable by name;
`OptimizeForBandwidth` optimizes resources in place without rewriting URLs;
`AllFilters` switches on every filter outside the dangerous set, including the
opt-in AVIF filters. `MobilizeFilters` and `TestingCoreFilters` exist for the
project's own testing. See [Filter selection](/docs/filter-selection/#rewritelevel).

#### RewriteRandomDropPercentage {#rewriterandomdroppercentage}

- **Syntax:** `ModPagespeedRewriteRandomDropPercentage number` (Apache) · `pagespeed RewriteRandomDropPercentage number;` (nginx) · `pagespeed RewriteRandomDropPercentage number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

The percentage of time that pagespeed should randomly drop an opportunity to optimize an image. The value should be an integer between 0 and 100 inclusive.

#### RewriteUncacheableResources {#rewriteuncacheableresources}

- **Syntax:** `ModPagespeedRewriteUncacheableResources on|off` (Apache) · `pagespeed RewriteUncacheableResources on|off;` (nginx) · `pagespeed RewriteUncacheableResources on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Allow optimization of uncacheable resources in the in-place rewriting mode.

### Request handling and policy {#area-request-handling-and-policy}

[`AcceptInvalidSignatures`](#acceptinvalidsignatures), [`AccessControlAllowOrigins`](#accesscontrolalloworigins), [`AddOptionsToUrls`](#addoptionstourls), [`AddResourceHeader`](#addresourceheader), [`AgentOptimize`](#agentoptimize), [`AllowLoggingUrlsInLogRecord`](#allowloggingurlsinlogrecord), [`AllowOptionsToBeSetByCookies`](#allowoptionstobesetbycookies), [`CombineAcrossPaths`](#combineacrosspaths), [`DisableBackgroundFetchesForBots`](#disablebackgroundfetchesforbots), [`DisableRewriteOnNoTransform`](#disablerewriteonnotransform), [`ExperimentalProxyAllRequests`](#experimentalproxyallrequests), [`ForceBuffering`](#forcebuffering), [`HideRefererUsingMeta`](#hiderefererusingmeta), [`HonorCsp`](#honorcsp), [`InlineResourcesWithoutExplicitAuthorization`](#inlineresourceswithoutexplicitauthorization), [`ModifyCachingHeaders`](#modifycachingheaders), [`ObliviousPagespeedUrls`](#obliviouspagespeedurls), [`OptionCookiesDurationMs`](#optioncookiesdurationms), [`PreserveSubresourceHints`](#preservesubresourcehints), [`ProxyAuth`](#proxyauth), [`RejectBlacklisted`](#rejectblacklisted), [`RejectBlacklistedStatusCode`](#rejectblacklistedstatuscode), [`RequestOptionOverride`](#requestoptionoverride), [`RespectVary`](#respectvary), [`RespectXForwardedProto`](#respectxforwardedproto), [`ServeStaleIfFetchError`](#servestaleiffetcherror), [`ServeStaleWhileRevalidateThresholdSec`](#servestalewhilerevalidatethresholdsec), [`ServeXhrAccessControlHeaders`](#servexhraccesscontrolheaders), [`StickyQueryParameters`](#stickyqueryparameters), [`SupportNoScriptEnabled`](#supportnoscriptenabled), [`TrackOriginalContentLength`](#trackoriginalcontentlength), [`UrlSigningKey`](#urlsigningkey), [`XHeaderValue`](#xheadervalue)

#### AcceptInvalidSignatures {#acceptinvalidsignatures}

- **Syntax:** `ModPagespeedAcceptInvalidSignatures on|off` (Apache) · `pagespeed AcceptInvalidSignatures on|off;` (nginx) · `pagespeed AcceptInvalidSignatures on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Accept resources with invalid signatures.

#### AccessControlAllowOrigins {#accesscontrolalloworigins}

- **Syntax:** `ModPagespeedAccessControlAllowOrigins value` (Apache) · `pagespeed AccessControlAllowOrigins value;` (nginx) · `pagespeed AccessControlAllowOrigins value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Comma separated list of origins that are allowed to make cross-origin requests

#### AddOptionsToUrls {#addoptionstourls}

- **Syntax:** `ModPagespeedAddOptionsToUrls on|off` (Apache) · `pagespeed AddOptionsToUrls on|off;` (nginx) · `pagespeed AddOptionsToUrls on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Add query-params with configuration adjustments to rewritten URLs.

#### AddResourceHeader {#addresourceheader}

- **Syntax:** `ModPagespeedAddResourceHeader name value` (Apache) · `pagespeed AddResourceHeader name value;` (nginx) · `pagespeed AddResourceHeader name value` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Adds a custom HTTP header to every optimized resource the module serves. Repeatable, up to 20 headers.

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

#### AgentOptimize {#agentoptimize}

- **Syntax:** `ModPagespeedAgentOptimize on|off` (Apache) · `pagespeed AgentOptimize on|off;` (nginx) · `pagespeed AgentOptimize on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Recognize Accept: text/markdown agent requests and add Vary: Accept on HTML. This flag is the only gate: no license or entitlement check sits behind it. 1.1 always serves optimized HTML (no markdown render).

#### AllowLoggingUrlsInLogRecord {#allowloggingurlsinlogrecord}

- **Syntax:** `pagespeed AllowLoggingUrlsInLogRecord on|off;` (nginx) · `pagespeed AllowLoggingUrlsInLogRecord on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### AllowOptionsToBeSetByCookies {#allowoptionstobesetbycookies}

- **Syntax:** `ModPagespeedAllowOptionsToBeSetByCookies on|off` (Apache) · `pagespeed AllowOptionsToBeSetByCookies on|off;` (nginx) · `pagespeed AllowOptionsToBeSetByCookies on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow options to be set by cookies in addition to query parameters and request headers.

#### CombineAcrossPaths {#combineacrosspaths}

- **Syntax:** `ModPagespeedCombineAcrossPaths on|off` (Apache) · `pagespeed CombineAcrossPaths on|off;` (nginx) · `pagespeed CombineAcrossPaths on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow combining resources from different paths

#### DisableBackgroundFetchesForBots {#disablebackgroundfetchesforbots}

- **Syntax:** `ModPagespeedDisableBackgroundFetchesForBots on|off` (Apache) · `pagespeed DisableBackgroundFetchesForBots on|off;` (nginx) · `pagespeed DisableBackgroundFetchesForBots on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Disable pre-emptive background fetches on bot requests.

#### DisableRewriteOnNoTransform {#disablerewriteonnotransform}

- **Syntax:** `ModPagespeedDisableRewriteOnNoTransform on|off` (Apache) · `pagespeed DisableRewriteOnNoTransform on|off;` (nginx) · `pagespeed DisableRewriteOnNoTransform on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

If false, resource is rewritten even if no-transform header is set

When enabled, resources served with `Cache-Control: no-transform` are left
alone.

#### ExperimentalProxyAllRequests {#experimentalproxyallrequests}

- **Syntax:** `ModPagespeedExperimentalProxyAllRequests on|off` (Apache)
- **Default:** `off`
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Experimental mode where mod_pagespeed acts entirely as a proxy, and doesn't attempt to work with any local serving.

#### ForceBuffering {#forcebuffering}

- **Syntax:** `ModPagespeedForceBuffering on|off` (Apache)
- **Default:** `off`
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Force buffering of non-html fetch responses rather than streaming

#### HideRefererUsingMeta {#hiderefererusingmeta}

- **Syntax:** `ModPagespeedHideRefererUsingMeta on|off` (Apache) · `pagespeed HideRefererUsingMeta on|off;` (nginx) · `pagespeed HideRefererUsingMeta on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Hides the referer by adding meta tag to the HTML

#### HonorCsp {#honorcsp}

- **Syntax:** `ModPagespeedHonorCsp on|off` (Apache) · `pagespeed HonorCsp on|off;` (nginx) · `pagespeed HonorCsp on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Controls whether PageSpeed should pay attention to Content-Security-Policy directives

Enabled by default. The module reads `Content-Security-Policy` response headers
and meta tags and suppresses optimizations the policy would block, such as
inlining resources or injecting scripts. Set it to `off` to optimize without
consulting CSP, and verify pages that rely on strict policies. Since
v1.15.0+r18 the filters that inject inline scripts also back off cleanly when
the policy disallows inline script.

#### InlineResourcesWithoutExplicitAuthorization {#inlineresourceswithoutexplicitauthorization}

- **Syntax:** `ModPagespeedInlineResourcesWithoutExplicitAuthorization value` (Apache) · `pagespeed InlineResourcesWithoutExplicitAuthorization value;` (nginx) · `pagespeed InlineResourcesWithoutExplicitAuthorization value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Specifies the resource types that can be inlined into HTML even if they do not belong to explicitly authorized domains.

#### ModifyCachingHeaders {#modifycachingheaders}

- **Syntax:** `ModPagespeedModifyCachingHeaders on|off` (Apache) · `pagespeed ModifyCachingHeaders on|off;` (nginx) · `pagespeed ModifyCachingHeaders on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Set to false to disallow mod_pagespeed from editing HTML Cache-Control headers. This is not safe in general and can cause the incorrect versions of HTML to be served to users. Rewritten HTML names the request headers its image URLs depend on (Accept, Save-Data, User-Agent) in Vary even when this is off, so a shared cache that honours Vary keeps the variants apart; caches that ignore Vary need their own cache key or a bypass for HTML.

Controls whether the module sets caching headers on HTML responses. Do not
disable it unless you fully understand the interaction with downstream caches:
disabling it can cause stale optimized content to be served.

#### ObliviousPagespeedUrls {#obliviouspagespeedurls}

- **Syntax:** `pagespeed ObliviousPagespeedUrls on|off;` (nginx) · `pagespeed ObliviousPagespeedUrls on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### OptionCookiesDurationMs {#optioncookiesdurationms}

- **Syntax:** `ModPagespeedOptionCookiesDurationMs number` (Apache) · `pagespeed OptionCookiesDurationMs number;` (nginx) · `pagespeed OptionCookiesDurationMs number` (IIS)
- **Default:** `600000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

The max-age in ms of cookies that set PageSpeed options.

#### PreserveSubresourceHints {#preservesubresourcehints}

- **Syntax:** `ModPagespeedPreserveSubresourceHints on|off` (Apache) · `pagespeed PreserveSubresourceHints on|off;` (nginx) · `pagespeed PreserveSubresourceHints on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Keep original subresource hints in place.

#### ProxyAuth {#proxyauth}

- **Syntax:** `ModPagespeedProxyAuth value` (Apache)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

CookieName[=Value][:RedirectUrl] -- checks proxy requests for CookieName. If CookieValue is specified, checks for that. If Redirect is specified, a failure results in a redirection to that URL otherwise a 403 is generated.

#### RejectBlacklisted {#rejectblacklisted}

- **Syntax:** `pagespeed RejectBlacklisted on|off;` (nginx) · `pagespeed RejectBlacklisted on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### RejectBlacklistedStatusCode {#rejectblacklistedstatuscode}

- **Syntax:** `pagespeed RejectBlacklistedStatusCode number;` (nginx) · `pagespeed RejectBlacklistedStatusCode number` (IIS)
- **Default:** `403`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### RequestOptionOverride {#requestoptionoverride}

- **Syntax:** `ModPagespeedRequestOptionOverride value` (Apache) · `pagespeed RequestOptionOverride value;` (nginx) · `pagespeed RequestOptionOverride value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Token passed in URL to enable pagespeed options in params.

#### RespectVary {#respectvary}

- **Syntax:** `ModPagespeedRespectVary on|off` (Apache) · `pagespeed RespectVary on|off;` (nginx) · `pagespeed RespectVary on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Whether to respect Vary headers for resources. Vary is always respected for HTML.

When enabled, resources whose `Vary` header indicates per-request variation are
not rewritten.

#### RespectXForwardedProto {#respectxforwardedproto}

- **Syntax:** `ModPagespeedRespectXForwardedProto on|off` (Apache) · `pagespeed RespectXForwardedProto on|off;` (nginx) · `pagespeed RespectXForwardedProto on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Whether to respect the X-Forwarded-Proto header.

#### ServeStaleIfFetchError {#servestaleiffetcherror}

- **Syntax:** `pagespeed ServeStaleIfFetchError on|off;` (nginx) · `pagespeed ServeStaleIfFetchError on|off` (IIS)
- **Default:** `on`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ServeStaleWhileRevalidateThresholdSec {#servestalewhilerevalidatethresholdsec}

- **Syntax:** `ModPagespeedServeStaleWhileRevalidateThresholdSec number` (Apache) · `pagespeed ServeStaleWhileRevalidateThresholdSec number;` (nginx) · `pagespeed ServeStaleWhileRevalidateThresholdSec number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Threshold for serving serving stale responses while revalidating in background. 0 means don't serve stale content.Note: Stale response will be served only for non-html requests.

#### ServeXhrAccessControlHeaders {#servexhraccesscontrolheaders}

- **Syntax:** `ModPagespeedServeXhrAccessControlHeaders on|off` (Apache) · `pagespeed ServeXhrAccessControlHeaders on|off;` (nginx) · `pagespeed ServeXhrAccessControlHeaders on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Serve access control headers with response headers

#### StickyQueryParameters {#stickyqueryparameters}

- **Syntax:** `ModPagespeedStickyQueryParameters value` (Apache) · `pagespeed StickyQueryParameters value;` (nginx) · `pagespeed StickyQueryParameters value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

The token that must be set by the PageSpeedStickyQueryParameters query parameter/header in a request to enable the setting of cookies for all other PageSpeed query parameters/headers in the request. Blank means it is disabled.

#### SupportNoScriptEnabled {#supportnoscriptenabled}

- **Syntax:** `ModPagespeedSupportNoScriptEnabled on|off` (Apache) · `pagespeed SupportNoScriptEnabled on|off;` (nginx) · `pagespeed SupportNoScriptEnabled on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Support for clients with no script support, in filters that insert new javascript.

#### TrackOriginalContentLength {#trackoriginalcontentlength}

- **Syntax:** `ModPagespeedTrackOriginalContentLength on|off` (Apache) · `pagespeed TrackOriginalContentLength on|off;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Add X-Original-Content-Length headers to rewritten resources

#### UrlSigningKey {#urlsigningkey}

- **Syntax:** `ModPagespeedUrlSigningKey value` (Apache) · `pagespeed UrlSigningKey value;` (nginx) · `pagespeed UrlSigningKey value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Key used for signing .pagespeed resource URLs.

#### XHeaderValue {#xheadervalue}

- **Syntax:** `ModPagespeedXHeaderValue value` (Apache) · `pagespeed XHeaderValue value;` (nginx) · `pagespeed XHeaderValue value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Set the value for the X-Mod-Pagespeed HTTP header

Sets the value of the `X-Mod-Pagespeed` (Apache) or `X-Page-Speed` (nginx, IIS)
[response header](/pagespeed-markers/#x-page-speed). From 2.7.0 the default is the generic
value `on`, which does not disclose the installed version. A value set here
takes precedence over the default and over
[`ExposeVersionInHeaders`](#expose-version-in-headers), the server-wide option that
restores the module version string.

### In-place resource optimization {#area-in-place-resource-optimization}

[`CacheSmallImagesUnrewritten`](#cachesmallimagesunrewritten), [`InPlacePreemptiveRewriteCss`](#inplacepreemptiverewritecss), [`InPlacePreemptiveRewriteCssImages`](#inplacepreemptiverewritecssimages), [`InPlacePreemptiveRewriteImages`](#inplacepreemptiverewriteimages), [`InPlacePreemptiveRewriteJavascript`](#inplacepreemptiverewritejavascript), [`InPlaceResourceOptimization`](#inplaceresourceoptimization), [`InPlaceRewriteDeadlineMs`](#inplacerewritedeadlinems), [`InPlaceSMaxAgeSec`](#inplacesmaxagesec), [`InPlaceWaitForOptimized`](#inplacewaitforoptimized), [`IproMaxConcurrentRecordings`](#ipromaxconcurrentrecordings), [`IproMaxResponseBytes`](#ipromaxresponsebytes), [`NoTransformOptimizedImages`](#notransformoptimizedimages), [`ProactivelyFreshenUserFacingRequest`](#proactivelyfreshenuserfacingrequest), [`ProactiveResourceFreshening`](#proactiveresourcefreshening)

#### CacheSmallImagesUnrewritten {#cachesmallimagesunrewritten}

- **Syntax:** `pagespeed CacheSmallImagesUnrewritten on|off;` (nginx) · `pagespeed CacheSmallImagesUnrewritten on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### InPlacePreemptiveRewriteCss {#inplacepreemptiverewritecss}

- **Syntax:** `ModPagespeedInPlacePreemptiveRewriteCss on|off` (Apache) · `pagespeed InPlacePreemptiveRewriteCss on|off;` (nginx) · `pagespeed InPlacePreemptiveRewriteCss on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

If set, issue preemptive rewrites of CSS on the HTML path when configured to use IPRO.

#### InPlacePreemptiveRewriteCssImages {#inplacepreemptiverewritecssimages}

- **Syntax:** `ModPagespeedInPlacePreemptiveRewriteCssImages on|off` (Apache) · `pagespeed InPlacePreemptiveRewriteCssImages on|off;` (nginx) · `pagespeed InPlacePreemptiveRewriteCssImages on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

If set, issue preemptive rewrites of CSS images on the IPRO serving path.

#### InPlacePreemptiveRewriteImages {#inplacepreemptiverewriteimages}

- **Syntax:** `ModPagespeedInPlacePreemptiveRewriteImages on|off` (Apache) · `pagespeed InPlacePreemptiveRewriteImages on|off;` (nginx) · `pagespeed InPlacePreemptiveRewriteImages on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

If set, issue preemptive rewrites of images on the HTML path when configured to use IPRO.

#### InPlacePreemptiveRewriteJavascript {#inplacepreemptiverewritejavascript}

- **Syntax:** `ModPagespeedInPlacePreemptiveRewriteJavascript on|off` (Apache) · `pagespeed InPlacePreemptiveRewriteJavascript on|off;` (nginx) · `pagespeed InPlacePreemptiveRewriteJavascript on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

If set, issue preemptive rewrites of JS on the HTML path when configured to use IPRO.

#### InPlaceResourceOptimization {#inplaceresourceoptimization}

- **Syntax:** `ModPagespeedInPlaceResourceOptimization on|off` (Apache) · `pagespeed InPlaceResourceOptimization on|off;` (nginx) · `pagespeed InPlaceResourceOptimization on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow rewriting resources even when they are fetched over non-pagespeed URLs.

#### InPlaceRewriteDeadlineMs {#inplacerewritedeadlinems}

- **Syntax:** `ModPagespeedInPlaceRewriteDeadlineMs number` (Apache) · `pagespeed InPlaceRewriteDeadlineMs number;` (nginx) · `pagespeed InPlaceRewriteDeadlineMs number` (IIS)
- **Default:** `10`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Time to wait for an in-place resource optimization beforefalling back to the original resource for the request.

#### InPlaceSMaxAgeSec {#inplacesmaxagesec}

- **Syntax:** `ModPagespeedInPlaceSMaxAgeSec number` (Apache) · `pagespeed InPlaceSMaxAgeSec number;` (nginx) · `pagespeed InPlaceSMaxAgeSec number` (IIS)
- **Default:** `10`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

What to set s-maxage to on not-yet-optimized ipro resources

#### InPlaceWaitForOptimized {#inplacewaitforoptimized}

- **Syntax:** `ModPagespeedInPlaceWaitForOptimized on|off` (Apache) · `pagespeed InPlaceWaitForOptimized on|off;` (nginx) · `pagespeed InPlaceWaitForOptimized on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Wait for optimizations to complete

#### IproMaxConcurrentRecordings {#ipromaxconcurrentrecordings}

- **Syntax:** `ModPagespeedIproMaxConcurrentRecordings number` (Apache) · `pagespeed IproMaxConcurrentRecordings number;` (nginx) · `pagespeed IproMaxConcurrentRecordings number` (IIS)
- **Default:** `10`
- **Context:** Apache: server config, virtual host (tolerated) · nginx: http · IIS: pagespeed.config, outside match rules

Limit allowed number of IPRO recordings

#### IproMaxResponseBytes {#ipromaxresponsebytes}

- **Syntax:** `ModPagespeedIproMaxResponseBytes number` (Apache) · `pagespeed IproMaxResponseBytes number;` (nginx) · `pagespeed IproMaxResponseBytes number` (IIS)
- **Default:** `10485760`
- **Context:** Apache: server config, virtual host (tolerated) · nginx: http · IIS: pagespeed.config, outside match rules

Limit allowed size of IPRO responses. Set to 0 for unlimited.

#### NoTransformOptimizedImages {#notransformoptimizedimages}

- **Syntax:** `ModPagespeedNoTransformOptimizedImages on|off` (Apache) · `pagespeed NoTransformOptimizedImages on|off;` (nginx) · `pagespeed NoTransformOptimizedImages on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Add no-transform header to cache-control for optimized images

#### ProactivelyFreshenUserFacingRequest {#proactivelyfreshenuserfacingrequest}

- **Syntax:** `pagespeed ProactivelyFreshenUserFacingRequest on|off;` (nginx) · `pagespeed ProactivelyFreshenUserFacingRequest on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ProactiveResourceFreshening {#proactiveresourcefreshening}

- **Syntax:** `ModPagespeedProactiveResourceFreshening on|off` (Apache) · `pagespeed ProactiveResourceFreshening on|off;` (nginx) · `pagespeed ProactiveResourceFreshening on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

If true, allows proactive freshening of inputs to the resource when they are close to expiry.

### Domains and URLs {#area-domains-and-urls}

[`ClientDomainRewrite`](#clientdomainrewrite), [`CssPreserveURLs`](#csspreserveurls), [`Domain`](#domain), [`DomainRewriteCookies`](#domainrewritecookies), [`DomainRewriteHyperlinks`](#domainrewritehyperlinks), [`DomainShardCount`](#domainshardcount), [`ImagePreserveURLs`](#imagepreserveurls), [`JsPreserveURLs`](#jspreserveurls), [`LoadFromFile`](#loadfromfile), [`LoadFromFileCacheTtlMs`](#loadfromfilecachettlms), [`LoadFromFileMatch`](#loadfromfilematch), [`LoadFromFileRule`](#loadfromfilerule), [`LoadFromFileRuleMatch`](#loadfromfilerulematch), [`MapOriginDomain`](#maporigindomain), [`MapProxyDomain`](#mapproxydomain), [`MapRewriteDomain`](#maprewritedomain), [`MaxSegmentLength`](#maxsegmentlength), [`MaxUrlSize`](#maxurlsize), [`PreserveUrlRelativity`](#preserveurlrelativity), [`ProxySuffix`](#proxysuffix), [`RemoteConfigurationTimeoutMs`](#remoteconfigurationtimeoutms), [`RemoteConfigurationUrl`](#remoteconfigurationurl), [`ShardDomain`](#sharddomain), [`StaticAssetCDN`](#staticassetcdn), [`StaticAssetPrefix`](#staticassetprefix), [`UrlValuedAttribute`](#urlvaluedattribute)

#### ClientDomainRewrite {#clientdomainrewrite}

- **Syntax:** `ModPagespeedClientDomainRewrite on|off` (Apache) · `pagespeed ClientDomainRewrite on|off;` (nginx) · `pagespeed ClientDomainRewrite on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow rewrite_domains to rewrite urls on the client side.

#### CssPreserveURLs {#csspreserveurls}

- **Syntax:** `ModPagespeedCssPreserveURLs on|off` (Apache) · `pagespeed CssPreserveURLs on|off;` (nginx) · `pagespeed CssPreserveURLs on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Disable the rewriting of CSS URLs.

#### Domain {#domain}

- **Syntax:** `ModPagespeedDomain domain` (Apache) · `pagespeed Domain domain;` (nginx) · `pagespeed Domain domain` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Authorizes the module to fetch and rewrite resources on this domain.

#### DomainRewriteCookies {#domainrewritecookies}

- **Syntax:** `ModPagespeedDomainRewriteCookies on|off` (Apache) · `pagespeed DomainRewriteCookies on|off;` (nginx) · `pagespeed DomainRewriteCookies on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow rewrite_domains to rewrite domains in Set-Cookie headers.

#### DomainRewriteHyperlinks {#domainrewritehyperlinks}

- **Syntax:** `ModPagespeedDomainRewriteHyperlinks on|off` (Apache) · `pagespeed DomainRewriteHyperlinks on|off;` (nginx) · `pagespeed DomainRewriteHyperlinks on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow rewrite_domains to rewrite &lt;form&gt; and &lt;a&gt; tags in addition to resource tags.

#### DomainShardCount {#domainshardcount}

- **Syntax:** `pagespeed DomainShardCount number;` (nginx) · `pagespeed DomainShardCount number` (IIS)
- **Default:** `0`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

_The module source carries no help text for this option._

#### ImagePreserveURLs {#imagepreserveurls}

- **Syntax:** `ModPagespeedImagePreserveURLs on|off` (Apache) · `pagespeed ImagePreserveURLs on|off;` (nginx) · `pagespeed ImagePreserveURLs on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Disable the rewriting of Image URLs.

#### JsPreserveURLs {#jspreserveurls}

- **Syntax:** `ModPagespeedJsPreserveURLs on|off` (Apache) · `pagespeed JsPreserveURLs on|off;` (nginx) · `pagespeed JsPreserveURLs on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Disable the rewriting of Javascript URLs.

#### LoadFromFile {#loadfromfile}

- **Syntax:** `ModPagespeedLoadFromFile url_prefix filename_prefix` (Apache) · `pagespeed LoadFromFile url_prefix filename_prefix;` (nginx) · `pagespeed LoadFromFile url_prefix filename_prefix` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Serves resources under a URL prefix straight from the filesystem instead of fetching them over HTTP. Access control: files served through a mapping are read directly from disk, so the web server's own access rules for those paths (Apache `<Directory>`, `<Files>` and `Require`; nginx `location` allow/deny and authentication) are not applied to them. To control access per path, use `LoadFromFileRule Disallow` for a filename prefix or `LoadFromFileRuleMatch Disallow` for a filename pattern; a disallowed file is fetched over HTTP instead, where the web server's rules apply as usual. If a path should not be reachable at all, do not map it.

#### LoadFromFileCacheTtlMs {#loadfromfilecachettlms}

- **Syntax:** `ModPagespeedLoadFromFileCacheTtlMs number` (Apache) · `pagespeed LoadFromFileCacheTtlMs number;` (nginx) · `pagespeed LoadFromFileCacheTtlMs number` (IIS)
- **Default:** `300000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Time in milliseconds to cache resources loaded from file that lack an Expires or Cache-Control header. If not explicitly set, defaults to using the value set by implicit_cache_ttl_ms

#### LoadFromFileMatch {#loadfromfilematch}

- **Syntax:** `ModPagespeedLoadFromFileMatch url_regexp filename_prefix` (Apache) · `pagespeed LoadFromFileMatch url_regexp filename_prefix;` (nginx) · `pagespeed LoadFromFileMatch url_regexp filename_prefix` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

LoadFromFile with a regular expression on the URL and a replacement filename prefix.

#### LoadFromFileRule {#loadfromfilerule}

- **Syntax:** `ModPagespeedLoadFromFileRule Allow|Disallow filename_prefix` (Apache) · `pagespeed LoadFromFileRule Allow|Disallow filename_prefix;` (nginx) · `pagespeed LoadFromFileRule Allow|Disallow filename_prefix` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Allows or disallows loading from file for a filename prefix after a LoadFromFile mapping.

#### LoadFromFileRuleMatch {#loadfromfilerulematch}

- **Syntax:** `ModPagespeedLoadFromFileRuleMatch Allow|Disallow filename_regexp` (Apache) · `pagespeed LoadFromFileRuleMatch Allow|Disallow filename_regexp;` (nginx) · `pagespeed LoadFromFileRuleMatch Allow|Disallow filename_regexp` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

LoadFromFileRule with a regular expression on the filename.

#### MapOriginDomain {#maporigindomain}

- **Syntax:** `ModPagespeedMapOriginDomain to_domain from_domain[,from_domain...] [host_header]` (Apache) · `pagespeed MapOriginDomain to_domain from_domain[,from_domain...] [host_header];` (nginx) · `pagespeed MapOriginDomain to_domain from_domain[,from_domain...] [host_header]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Fetches resources for the public domain(s) from the origin domain instead, optionally sending a different Host header.

#### MapProxyDomain {#mapproxydomain}

- **Syntax:** `ModPagespeedMapProxyDomain proxy_domain origin_domain [to_domain]` (Apache) · `pagespeed MapProxyDomain proxy_domain origin_domain [to_domain];` (nginx) · `pagespeed MapProxyDomain proxy_domain origin_domain [to_domain]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Proxies and optimizes resources from a separate origin domain under a path of this site.

#### MapRewriteDomain {#maprewritedomain}

- **Syntax:** `ModPagespeedMapRewriteDomain to_domain from_domain[,from_domain...]` (Apache) · `pagespeed MapRewriteDomain to_domain from_domain[,from_domain...];` (nginx) · `pagespeed MapRewriteDomain to_domain from_domain[,from_domain...]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Rewrites optimized resource URLs from the listed domains onto another domain (for example a CDN hostname).

#### MaxSegmentLength {#maxsegmentlength}

- **Syntax:** `ModPagespeedMaxSegmentLength number` (Apache) · `pagespeed MaxSegmentLength number;` (nginx) · `pagespeed MaxSegmentLength number` (IIS)
- **Default:** `1024`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Maximum size of a URL segment.

Why and when to lower it is in [URL segment length limits](#max-url-segments).

#### MaxUrlSize {#maxurlsize}

- **Syntax:** `pagespeed MaxUrlSize number;` (nginx) · `pagespeed MaxUrlSize number` (IIS)
- **Default:** `2083`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### PreserveUrlRelativity {#preserveurlrelativity}

- **Syntax:** `ModPagespeedPreserveUrlRelativity on|off` (Apache) · `pagespeed PreserveUrlRelativity on|off;` (nginx) · `pagespeed PreserveUrlRelativity on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Keep rewritten URLs as relative as the original resource URL was.

When enabled, relative URLs in rewritten HTML stay relative instead of being
converted to absolute URLs.

#### ProxySuffix {#proxysuffix}

- **Syntax:** `ModPagespeedProxySuffix suffix` (Apache) · `pagespeed ProxySuffix suffix;` (nginx) · `pagespeed ProxySuffix suffix` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Sets up a proxy suffix to be used when slurping.

#### RemoteConfigurationTimeoutMs {#remoteconfigurationtimeoutms}

- **Syntax:** `ModPagespeedRemoteConfigurationTimeoutMs number` (Apache) · `pagespeed RemoteConfigurationTimeoutMs number;` (nginx) · `pagespeed RemoteConfigurationTimeoutMs number` (IIS)
- **Default:** `1000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Timeout for fetch of remote configuration file.

#### RemoteConfigurationUrl {#remoteconfigurationurl}

- **Syntax:** `ModPagespeedRemoteConfigurationUrl value` (Apache) · `pagespeed RemoteConfigurationUrl value;` (nginx) · `pagespeed RemoteConfigurationUrl value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

URL of site from which to pull remote configuration files

#### ShardDomain {#sharddomain}

- **Syntax:** `ModPagespeedShardDomain from_domain shard_domain[,shard_domain...]` (Apache) · `pagespeed ShardDomain from_domain shard_domain[,shard_domain...];` (nginx) · `pagespeed ShardDomain from_domain shard_domain[,shard_domain...]` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Spreads optimized resource URLs for a domain across the listed shard domains.

#### StaticAssetCDN {#staticassetcdn}

- **Syntax:** `ModPagespeedStaticAssetCDN value` (Apache) · `pagespeed StaticAssetCDN value;` (nginx) · `pagespeed StaticAssetCDN value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Configures serving of helper scripts from external URLs rather than from compiled-in versions via static handler.

#### StaticAssetPrefix {#staticassetprefix}

- **Syntax:** `ModPagespeedStaticAssetPrefix url_prefix` (Apache) · `pagespeed StaticAssetPrefix url_prefix;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Where to serve static support files for pagespeed filters from.

URL prefix the module serves its own static assets from (the JavaScript
libraries and images some filters inject). The default is `/pagespeed_static/`.

#### UrlValuedAttribute {#urlvaluedattribute}

- **Syntax:** `ModPagespeedUrlValuedAttribute element attribute category` (Apache) · `pagespeed UrlValuedAttribute element attribute category;` (nginx) · `pagespeed UrlValuedAttribute element attribute category` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Declares an extra element attribute that holds a URL, with the resource category (image, script, stylesheet, otherResource, hyperlink) it should be treated as.

### Optimizer worker {#area-optimizer-worker}

[`DaemonApiSocketPath`](#daemonapisocketpath), [`DaemonHtmlCache`](#daemonhtmlcache), [`DaemonHtmlMaxBytes`](#daemonhtmlmaxbytes), [`DaemonServeHonorRequestFreshness`](#daemonservehonorrequestfreshness), [`DaemonServeStoredEncodings`](#daemonservestoredencodings), [`DaemonSocketPath`](#daemonsocketpath), [`DaemonVolumePath`](#daemonvolumepath)

#### DaemonApiSocketPath {#daemonapisocketpath}

- **Syntax:** `ModPagespeedDaemonApiSocketPath value` (Apache) · `pagespeed DaemonApiSocketPath value;` (nginx)
- **Default:** `/run/pagespeed-optimizer/api.sock`
- **Context:** Apache: server config, virtual host · nginx: http, server

Path of the optimizer worker's management API unix socket, backing the /v1/daemon/\* admin endpoints. Empty disables them. This is a different socket from DaemonSocketPath (the notification socket).

On nginx: Set the unix socket path of the optimizer worker's management API, backing the /v1/daemon/\* admin endpoints. Empty disables them.

#### DaemonHtmlCache {#daemonhtmlcache}

- **Syntax:** `ModPagespeedDaemonHtmlCache off|report-only` (Apache) · `pagespeed DaemonHtmlCache off|report-only;` (nginx) · `pagespeed DaemonHtmlCache off|report-only` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

HTML on the optimizer worker path: off (default), or report-only, which reports whether each HTML response could be stored and changes nothing else.

#### DaemonHtmlMaxBytes {#daemonhtmlmaxbytes}

- **Syntax:** `ModPagespeedDaemonHtmlMaxBytes number` (Apache) · `pagespeed DaemonHtmlMaxBytes number;` (nginx) · `pagespeed DaemonHtmlMaxBytes number` (IIS)
- **Default:** `5242880`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Largest HTML body DaemonHtmlCache considers storable, in bytes. Values above 16 MiB count as 16 MiB.

#### DaemonServeHonorRequestFreshness {#daemonservehonorrequestfreshness}

- **Syntax:** `ModPagespeedDaemonServeHonorRequestFreshness on|off` (Apache) · `pagespeed DaemonServeHonorRequestFreshness on|off;` (nginx)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server

Let a client's reload headers (Cache-Control: no-cache or max-age=0, or Pragma: no-cache without a Cache-Control) bypass the optimizer worker's cached copy of a resource, so the origin's own bytes are sent. Default off: a reload is served from the cache.

#### DaemonServeStoredEncodings {#daemonservestoredencodings}

- **Syntax:** `ModPagespeedDaemonServeStoredEncodings on|off` (Apache) · `pagespeed DaemonServeStoredEncodings on|off;` (nginx)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server

Serve the optimizer worker's stored gzip and brotli copies of stylesheets, scripts and SVG images to clients that accept them, labelled with Content-Encoding, instead of compressing the uncompressed copy on the way out. Default off.

#### DaemonSocketPath {#daemonsocketpath}

- **Syntax:** `ModPagespeedDaemonSocketPath value` (Apache) · `pagespeed DaemonSocketPath value;` (nginx)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server

Path of the optimizer worker's notification socket. Set this together with DaemonVolumePath to hand in-place optimization to the worker; leave both unset to keep the classic in-place path.

On nginx: Path of the optimizer worker's notification socket. Set this together with DaemonVolumePath; leave both unset to keep the classic in-place path. With both set, the nginx port records through the worker and serves the optimized copies from its shared cache.

Set together with `DaemonVolumePath` to hand in-place optimization to the
optimizer worker installed beside the module. The worker's own flags are on
[Worker and reverse-proxy configuration](/docs/worker-configuration/).

#### DaemonVolumePath {#daemonvolumepath}

- **Syntax:** `ModPagespeedDaemonVolumePath value` (Apache) · `pagespeed DaemonVolumePath value;` (nginx)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server

Path of the optimizer worker's shared cache volume. Keep this on a DIFFERENT path from FileCachePath. Set this together with DaemonSocketPath; leave both unset to keep the classic in-place path.

On nginx: Path of the optimizer worker's shared cache volume. Keep this on a DIFFERENT path from FileCachePath. Set this together with DaemonSocketPath; leave both unset to keep the classic in-place path. With both set, the nginx port records through the worker and serves the optimized copies from its shared cache.

Keep this on a different path from `FileCachePath`. The worker creates the
volume under its `--cache-dir`; see
[Worker and reverse-proxy configuration](/docs/worker-configuration/#--cache-dir).

### Rule packs {#area-rule-packs}

[`RulePackFile`](#rulepackfile), [`RulePackMode`](#rulepackmode), [`RulePackRequireSigned`](#rulepackrequiresigned), [`RulePackTrustKey`](#rulepacktrustkey)

#### RulePackFile {#rulepackfile}

- **Syntax:** `ModPagespeedRulePackFile value` (Apache) · `pagespeed RulePackFile value;` (nginx) · `pagespeed RulePackFile value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Path of the rule pack file the rule_packs filter applies.

Repeatable: each line adds a pack file, up to eight per server or virtual host
(a ninth is a configuration error, and two lines naming the same path count
once). The packs run in the order of the lines. A virtual host that sets
`RulePackFile` uses its own list instead of the server-wide one. See
[Rule packs](/docs/rule-packs/#enable).

#### RulePackMode {#rulepackmode}

- **Syntax:** `ModPagespeedRulePackMode report|enforce` (Apache) · `pagespeed RulePackMode report|enforce;` (nginx) · `pagespeed RulePackMode report|enforce` (IIS)
- **Default:** `report`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Mode of the rule_packs filter: report (default) records what the rules would do; enforce lets enforcing rules change the page.

#### RulePackRequireSigned {#rulepackrequiresigned}

- **Syntax:** `ModPagespeedRulePackRequireSigned on|off` (Apache) · `pagespeed RulePackRequireSigned on|off;` (nginx) · `pagespeed RulePackRequireSigned on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Refuse rule packs that are not signed with a key listed by RulePackTrustKey.

#### RulePackTrustKey {#rulepacktrustkey}

- **Syntax:** `ModPagespeedRulePackTrustKey key_id base64_public_key` (Apache) · `pagespeed RulePackTrustKey key_id base64_public_key;` (nginx) · `pagespeed RulePackTrustKey key_id base64_public_key` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Trust rule packs signed with this key: &lt;key id&gt; &lt;base64 Ed25519 public key&gt;

### Bot authentication and licensing {#area-bot-authentication-and-licensing}

[`RslCapDirectoryHost`](#rslcapdirectoryhost), [`RslCapEnforcement`](#rslcapenforcement), [`RslCapIssuer`](#rslcapissuer), [`RslCapKeyDirectoryAllowlist`](#rslcapkeydirectoryallowlist), [`RslCapKeyDirectoryFile`](#rslcapkeydirectoryfile), [`RslCapKeyDirectoryRefreshSec`](#rslcapkeydirectoryrefreshsec), [`RslCapKeyDirectoryUrl`](#rslcapkeydirectoryurl), [`RslCapRequestedLicense`](#rslcaprequestedlicense), [`RslCapRequestedScope`](#rslcaprequestedscope), [`WebBotAuth`](#webbotauth), [`WebBotAuthBotDetection`](#webbotauthbotdetection), [`WebBotAuthDirectoryHost`](#webbotauthdirectoryhost), [`WebBotAuthKeyDirectoryAllowlist`](#webbotauthkeydirectoryallowlist), [`WebBotAuthKeyDirectoryFile`](#webbotauthkeydirectoryfile), [`WebBotAuthKeyDirectoryRefreshSec`](#webbotauthkeydirectoryrefreshsec), [`WebBotAuthKeyDirectoryUrl`](#webbotauthkeydirectoryurl), [`WebBotAuthPublicCounter`](#webbotauthpubliccounter), [`WebBotAuthTelemetry`](#webbotauthtelemetry), [`WebBotAuthVerifiedBots`](#webbotauthverifiedbots)

#### RslCapDirectoryHost {#rslcapdirectoryhost}

- **Syntax:** `pagespeed RslCapDirectoryHost value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Operator-mapped issuer directory host passed to the key provider (plane-split, never request-derived). Ignored by the v1 static-file provider; used by the follow-up network provider. Default empty.

#### RslCapEnforcement {#rslcapenforcement}

- **Syntax:** `pagespeed RslCapEnforcement on|off;` (nginx)
- **Default:** `off`
- **Context:** nginx: http, server
- **Platform:** nginx only

Enable RSL-CAP capability-token enforcement: an Authorization: License token is validated and the verdict is mapped to an inline 401/402. Never settles/meters. Default off.

See [RSL-CAP enforcement](/docs/rsl-cap/).

#### RslCapIssuer {#rslcapissuer}

- **Syntax:** `pagespeed RslCapIssuer value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Optional issuer pin: when set, an otherwise-authorized token whose iss != this value is rejected (401). Use when a directory host may serve multiple issuers. Default empty.

#### RslCapKeyDirectoryAllowlist {#rslcapkeydirectoryallowlist}

- **Syntax:** `pagespeed RslCapKeyDirectoryAllowlist value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

SSRF allowlist: comma-separated https origins the RSL-CAP warm-fetch may contact. Empty =&gt; warm-fetch disabled (fail-closed). Default empty.

#### RslCapKeyDirectoryFile {#rslcapkeydirectoryfile}

- **Syntax:** `pagespeed RslCapKeyDirectoryFile value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Path to a local JWKS file (the issuer's published key directory, copied locally by the operator) used to resolve RSL-CAP signing keys. v1: synchronous, no network fetch. Default empty.

#### RslCapKeyDirectoryRefreshSec {#rslcapkeydirectoryrefreshsec}

- **Syntax:** `pagespeed RslCapKeyDirectoryRefreshSec value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

RSL-CAP warm-fetch refresh interval in seconds (clamped). Empty =&gt; default 3600. Default empty.

#### RslCapKeyDirectoryUrl {#rslcapkeydirectoryurl}

- **Syntax:** `pagespeed RslCapKeyDirectoryUrl value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

HTTPS URL of the issuer's JWKS directory, fetched off-request by a background thread and cached; the request path reads cache-only. Requires RslCapKeyDirectoryAllowlist and RslCapDirectoryHost. Default empty (no network fetch).

#### RslCapRequestedLicense {#rslcaprequestedlicense}

- **Syntax:** `pagespeed RslCapRequestedLicense value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

License id this route requires; a token must grant it (and the requested scope) to be authorized, else 402. Default empty.

#### RslCapRequestedScope {#rslcaprequestedscope}

- **Syntax:** `pagespeed RslCapRequestedScope value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Scope this route requires; a token must grant it (and the requested license) to be authorized, else 402. Default empty.

#### WebBotAuth {#webbotauth}

- **Syntax:** `pagespeed WebBotAuth on|off;` (nginx)
- **Default:** `off`
- **Context:** nginx: http, server
- **Platform:** nginx only

Enable observe-only Web-Bot-Auth (RFC 9421) request classification, surfaced as $x_verified_bot. Never blocks. Default off.

#### WebBotAuthBotDetection {#webbotauthbotdetection}

- **Syntax:** `pagespeed WebBotAuthBotDetection on|off;` (nginx)
- **Default:** `off`
- **Context:** nginx: http, server
- **Platform:** nginx only

Let a verified Web-Bot-Auth signature classify the request as an automated client for PageSpeed's own bot detection, so a signed agent is recognised even when it presents a browser user-agent. Suppresses the measurement beacons and lazyload for that request; never blocks it. Requires WebBotAuth. Default off (verdict stays observe-only).

Opt-in: a cryptographically verified Web Bot Auth signature (RFC 9421)
classifies the request as automated for the module's own bot detection, so a
signed agent is recognized even when it presents a browser user agent. Requires
`WebBotAuth`. See [Web Bot Auth](/docs/web-bot-auth/).

#### WebBotAuthDirectoryHost {#webbotauthdirectoryhost}

- **Syntax:** `pagespeed WebBotAuthDirectoryHost value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

The signer directory's host identity: the warm-fetch cache key, paired with WebBotAuthKeyDirectoryUrl. Ignored by the local-file static provider. Never request-derived. Default empty.

#### WebBotAuthKeyDirectoryAllowlist {#webbotauthkeydirectoryallowlist}

- **Syntax:** `pagespeed WebBotAuthKeyDirectoryAllowlist value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

SSRF allowlist: comma-separated https origins (scheme://host[:port]) the warm-fetch may contact. Empty =&gt; warm-fetch disabled (fail-closed). Default empty.

#### WebBotAuthKeyDirectoryFile {#webbotauthkeydirectoryfile}

- **Syntax:** `pagespeed WebBotAuthKeyDirectoryFile value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Path to a local JWKS file (the signer's published key directory, copied locally by the operator) used to verify signatures. A1 v1: no network fetch. Default empty.

#### WebBotAuthKeyDirectoryRefreshSec {#webbotauthkeydirectoryrefreshsec}

- **Syntax:** `pagespeed WebBotAuthKeyDirectoryRefreshSec value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Warm-fetch refresh interval in seconds (clamped). Empty =&gt; default 3600. Default empty.

#### WebBotAuthKeyDirectoryUrl {#webbotauthkeydirectoryurl}

- **Syntax:** `pagespeed WebBotAuthKeyDirectoryUrl value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

HTTPS URL of the signer's JWKS directory, fetched off-request by a background thread and cached; the request path reads cache-only and never fetches. Requires WebBotAuthKeyDirectoryAllowlist and WebBotAuthDirectoryHost. Default empty (no network fetch).

#### WebBotAuthPublicCounter {#webbotauthpubliccounter}

- **Syntax:** `pagespeed WebBotAuthPublicCounter value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Opt-in verified-crawl counter mode (experimental): off (default) | private | public. Enabling a non-off mode publishes a discoverable marker at /.well-known/webbotauth-counter; the exact document is gated by the PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN env bearer token. Default off (endpoint invisible).

#### WebBotAuthTelemetry {#webbotauthtelemetry}

- **Syntax:** `pagespeed WebBotAuthTelemetry on|off;` (nginx)
- **Default:** `off`
- **Context:** nginx: http, server
- **Platform:** nginx only

Count verified/signed-agent requests in the opt-in web_bot_auth_verified_signed_requests statistic (and non-web-bot-auth signature material in web_bot_auth_other_signature_requests). Default off.

#### WebBotAuthVerifiedBots {#webbotauthverifiedbots}

- **Syntax:** `pagespeed WebBotAuthVerifiedBots value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Verified-bot registry: comma-separated keyid=name pairs.

### Statistics, console and logging {#area-statistics-console-and-logging}

[`AdminDomains`](#admindomains), [`AdminPath`](#adminpath), [`AutoCreateLogDir`](#autocreatelogdir), [`ConsoleDomains`](#consoledomains), [`ConsolePath`](#consolepath), [`GlobalAdminDomains`](#globaladmindomains), [`GlobalAdminPath`](#globaladminpath), [`GlobalStatisticsDomains`](#globalstatisticsdomains), [`GlobalStatisticsPath`](#globalstatisticspath), [`InfoUrlsLocalOnly`](#infourlslocalonly), [`InstallCrashHandler`](#installcrashhandler), [`ListOutstandingUrlsOnError`](#listoutstandingurlsonerror), [`LogBackgroundRewrite`](#logbackgroundrewrite), [`LogDir`](#logdir), [`LogRewriteTiming`](#logrewritetiming), [`LogUrlIndices`](#logurlindices), [`MaxRewriteInfoLogSize`](#maxrewriteinfologsize), [`MessageBufferSize`](#messagebuffersize), [`MessagesDomains`](#messagesdomains), [`MessagesPath`](#messagespath), [`ReportUnloadTime`](#reportunloadtime), [`SlowFileLatencyUs`](#slowfilelatencyus), [`Statistics`](#statistics), [`StatisticsDomains`](#statisticsdomains), [`StatisticsLogging`](#statisticslogging), [`StatisticsLoggingChartsCSS`](#statisticsloggingchartscss), [`StatisticsLoggingChartsJS`](#statisticsloggingchartsjs), [`StatisticsLoggingIntervalMs`](#statisticsloggingintervalms), [`StatisticsLoggingMaxFileSizeKb`](#statisticsloggingmaxfilesizekb), [`StatisticsPath`](#statisticspath), [`StrictAdminAccess`](#strictadminaccess), [`TreatProxiedRequestsAsLocal`](#treatproxiedrequestsaslocal), [`UsePerVHostStatistics`](#usepervhoststatistics)

#### AdminDomains {#admindomains}

- **Syntax:** `ModPagespeedAdminDomains Allow|Disallow domain_wildcard` (Apache) · `pagespeed AdminDomains Allow|Disallow domain_wildcard;` (nginx) · `pagespeed AdminDomains Allow|Disallow domain_wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server, location · IIS: pagespeed.config, outside match rules

Access control for the per-host admin pages: Allow or Disallow a domain wildcard. Default: Allow \*.

#### AdminPath {#adminpath}

- **Syntax:** `pagespeed AdminPath value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Set the admin path. Ex: /pagespeed_admin

#### AutoCreateLogDir {#autocreatelogdir}

- **Syntax:** `pagespeed AutoCreateLogDir on|off` (IIS)
- **Default:** `on`
- **Context:** IIS: pagespeed.config, outside match rules
- **Platform:** IIS only

Auto-create the LogDir if missing (on|off, default on).

#### ConsoleDomains {#consoledomains}

- **Syntax:** `ModPagespeedConsoleDomains Allow|Disallow domain_wildcard` (Apache) · `pagespeed ConsoleDomains Allow|Disallow domain_wildcard;` (nginx) · `pagespeed ConsoleDomains Allow|Disallow domain_wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server, location · IIS: pagespeed.config, outside match rules

Access control for the console page: Allow or Disallow a domain wildcard. Default: Allow \*.

#### ConsolePath {#consolepath}

- **Syntax:** `pagespeed ConsolePath value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Set the console path. Ex: /pagespeed_console

#### GlobalAdminDomains {#globaladmindomains}

- **Syntax:** `ModPagespeedGlobalAdminDomains Allow|Disallow domain_wildcard` (Apache) · `pagespeed GlobalAdminDomains Allow|Disallow domain_wildcard;` (nginx) · `pagespeed GlobalAdminDomains Allow|Disallow domain_wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server, location · IIS: pagespeed.config, outside match rules

Access control for the global admin pages: Allow or Disallow a domain wildcard. Default: Allow \*.

#### GlobalAdminPath {#globaladminpath}

- **Syntax:** `pagespeed GlobalAdminPath value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http
- **Platform:** nginx only

Set the global admin path. Ex: /pagespeed_global_admin

#### GlobalStatisticsDomains {#globalstatisticsdomains}

- **Syntax:** `ModPagespeedGlobalStatisticsDomains Allow|Disallow domain_wildcard` (Apache) · `pagespeed GlobalStatisticsDomains Allow|Disallow domain_wildcard;` (nginx) · `pagespeed GlobalStatisticsDomains Allow|Disallow domain_wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server, location · IIS: pagespeed.config, outside match rules

Access control for the global statistics page: Allow or Disallow a domain wildcard. Default: Allow \*.

#### GlobalStatisticsPath {#globalstatisticspath}

- **Syntax:** `pagespeed GlobalStatisticsPath value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http
- **Platform:** nginx only

Set the global statistics path. Ex: /ngx_pagespeed_global_statistics

#### InfoUrlsLocalOnly {#infourlslocalonly}

- **Syntax:** `pagespeed InfoUrlsLocalOnly on|off` (IIS)
- **Default:** `on`
- **Context:** IIS: pagespeed.config (a match rule does not limit it)
- **Platform:** IIS only

Serve the admin (including cache purge), statistics, console and message pages to local requests only. With off, they are served to every request, unless an allowlist (AdminDomains, StatisticsDomains, ConsoleDomains, MessagesDomains, GlobalAdminDomains and GlobalStatisticsDomains) is set for that page; the request's host name (its Host header) must then match it. A request whose host name is on a configured allowlist is served even when it is not local. The value applies to the whole IIS process, not to one site.

A request is local when it comes from the server itself and carries no
reverse-proxy forwarding headers; `TreatProxiedRequestsAsLocal` changes that
for one site. See [Admin console access control](/docs/admin-console/#access-control).

#### InstallCrashHandler {#installcrashhandler}

- **Syntax:** `ModPagespeedInstallCrashHandler on|off` (Apache) · `pagespeed InstallCrashHandler on|off;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Try to dump backtrace on crashes. For developer use

#### ListOutstandingUrlsOnError {#listoutstandingurlsonerror}

- **Syntax:** `ModPagespeedListOutstandingUrlsOnError on|off` (Apache) · `pagespeed ListOutstandingUrlsOnError on|off;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Adds an error message into the log for every URL fetch in flight when the HTTP stack encounters a system error, e.g. Connection Refused

Enable it only while debugging fetch problems; it is noisy in production.

#### LogBackgroundRewrite {#logbackgroundrewrite}

- **Syntax:** `pagespeed LogBackgroundRewrite on|off;` (nginx) · `pagespeed LogBackgroundRewrite on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server · IIS: pagespeed.config, outside match rules

_The module source carries no help text for this option._

#### LogDir {#logdir}

- **Syntax:** `ModPagespeedLogDir value` (Apache) · `pagespeed LogDir value;` (nginx) · `pagespeed LogDir value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Directory to store logs in.

#### LogRewriteTiming {#logrewritetiming}

- **Syntax:** `ModPagespeedLogRewriteTiming on|off` (Apache) · `pagespeed LogRewriteTiming on|off;` (nginx) · `pagespeed LogRewriteTiming on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Whether or not to report timing information about HtmlParse.

#### LogUrlIndices {#logurlindices}

- **Syntax:** `ModPagespeedLogUrlIndices on|off` (Apache) · `pagespeed LogUrlIndices on|off;` (nginx) · `pagespeed LogUrlIndices on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Whether or not to log URL indices for rewriter applications.

#### MaxRewriteInfoLogSize {#maxrewriteinfologsize}

- **Syntax:** `pagespeed MaxRewriteInfoLogSize number;` (nginx) · `pagespeed MaxRewriteInfoLogSize number` (IIS)
- **Default:** `150`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### MessageBufferSize {#messagebuffersize}

- **Syntax:** `ModPagespeedMessageBufferSize bytes` (Apache) · `pagespeed MessageBufferSize bytes;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Size in bytes of the in-memory message history shown on the messages page; 0 disables it.

#### MessagesDomains {#messagesdomains}

- **Syntax:** `ModPagespeedMessagesDomains Allow|Disallow domain_wildcard` (Apache) · `pagespeed MessagesDomains Allow|Disallow domain_wildcard;` (nginx) · `pagespeed MessagesDomains Allow|Disallow domain_wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server, location · IIS: pagespeed.config, outside match rules

Access control for the message history page: Allow or Disallow a domain wildcard. Default: Allow \*.

#### MessagesPath {#messagespath}

- **Syntax:** `pagespeed MessagesPath value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Set the messages path. Ex: /ngx_pagespeed_message

#### ReportUnloadTime {#reportunloadtime}

- **Syntax:** `ModPagespeedReportUnloadTime on|off` (Apache) · `pagespeed ReportUnloadTime on|off;` (nginx) · `pagespeed ReportUnloadTime on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated, no effect: the beacon is now sent on page-hide, which covers pre-onload abandonment without breaking bfcache.

#### SlowFileLatencyUs {#slowfilelatencyus}

- **Syntax:** `ModPagespeedSlowFileLatencyUs number` (Apache) · `pagespeed SlowFileLatencyUs number;` (nginx) · `pagespeed SlowFileLatencyUs number` (IIS)
- **Default:** `50000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Maximum time in microseconds to allow for file operations before logging and bumping a stat

#### Statistics {#statistics}

- **Syntax:** `ModPagespeedStatistics on|off` (Apache) · `pagespeed Statistics on|off;` (nginx) · `pagespeed Statistics on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Whether to collect cross-process statistics.

#### StatisticsDomains {#statisticsdomains}

- **Syntax:** `ModPagespeedStatisticsDomains Allow|Disallow domain_wildcard` (Apache) · `pagespeed StatisticsDomains Allow|Disallow domain_wildcard;` (nginx) · `pagespeed StatisticsDomains Allow|Disallow domain_wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server, location · IIS: pagespeed.config, outside match rules

Access control for the per-host statistics page: Allow or Disallow a domain wildcard. Default: Allow \*.

#### StatisticsLogging {#statisticslogging}

- **Syntax:** `ModPagespeedStatisticsLogging on|off` (Apache) · `pagespeed StatisticsLogging on|off;` (nginx) · `pagespeed StatisticsLogging on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Whether to log statistics if they're being collected.

#### StatisticsLoggingChartsCSS {#statisticsloggingchartscss}

- **Syntax:** `ModPagespeedStatisticsLoggingChartsCSS value` (Apache) · `pagespeed StatisticsLoggingChartsCSS value;` (nginx) · `pagespeed StatisticsLoggingChartsCSS value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Where to find an offline copy of the Google Charts Tools API CSS.

#### StatisticsLoggingChartsJS {#statisticsloggingchartsjs}

- **Syntax:** `ModPagespeedStatisticsLoggingChartsJS value` (Apache) · `pagespeed StatisticsLoggingChartsJS value;` (nginx) · `pagespeed StatisticsLoggingChartsJS value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Where to find an offline copy of the Google Charts Tools API JS.

#### StatisticsLoggingIntervalMs {#statisticsloggingintervalms}

- **Syntax:** `ModPagespeedStatisticsLoggingIntervalMs number` (Apache) · `pagespeed StatisticsLoggingIntervalMs number;` (nginx) · `pagespeed StatisticsLoggingIntervalMs number` (IIS)
- **Default:** `600000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

How often to log statistics, in milliseconds.

#### StatisticsLoggingMaxFileSizeKb {#statisticsloggingmaxfilesizekb}

- **Syntax:** `ModPagespeedStatisticsLoggingMaxFileSizeKb number` (Apache) · `pagespeed StatisticsLoggingMaxFileSizeKb number;` (nginx) · `pagespeed StatisticsLoggingMaxFileSizeKb number` (IIS)
- **Default:** `1024`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Max size for statistics logging file.

#### StatisticsPath {#statisticspath}

- **Syntax:** `pagespeed StatisticsPath value;` (nginx)
- **Default:** `(empty)`
- **Context:** nginx: http, server
- **Platform:** nginx only

Set the statistics path. Ex: /ngx_pagespeed_statistics

#### StrictAdminAccess {#strictadminaccess}

- **Syntax:** `ModPagespeedStrictAdminAccess on|off` (Apache) · `pagespeed StrictAdminAccess on|off;` (nginx) · `pagespeed StrictAdminAccess on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

OPT-IN, default off. When on, the admin, statistics, console and message handlers deny non-loopback clients unless an explicit \*Domains allowlist is configured. The loopback decision uses the validated client connection IP, not the Host header. When off (default) access checks behave exactly as before.

#### TreatProxiedRequestsAsLocal {#treatproxiedrequestsaslocal}

- **Syntax:** `pagespeed TreatProxiedRequestsAsLocal on|off` (IIS)
- **Default:** `off`
- **Context:** IIS: pagespeed.config, outside match rules
- **Platform:** IIS only

Count a request relayed by a reverse proxy or tunnel on this server (one that carries forwarding headers such as X-Forwarded-For) as local for the admin, statistics, console and message pages and the diagnostic page. Default off: only direct local requests are local. Use AdminDomains and StatisticsDomains to allow named hosts instead.

Turning it on serves these pages to anyone who reaches the site through that
proxy or tunnel; prefer the `*Domains` allowlists.

#### UsePerVHostStatistics {#usepervhoststatistics}

- **Syntax:** `ModPagespeedUsePerVHostStatistics on|off` (Apache) · `pagespeed UsePerVHostStatistics on|off;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

If true, keep track of statistics per VHost and not just globally

### Images {#area-images}

[`AvifAnimatedRecompressionQuality`](#avifanimatedrecompressionquality), [`AvifQualityForSaveData`](#avifqualityforsavedata), [`AvifRecompressionQuality`](#avifrecompressionquality), [`AvifRecompressionQualityForSmallScreens`](#avifrecompressionqualityforsmallscreens), [`AvifTimeoutMs`](#aviftimeoutms), [`CriticalImagesBeaconEnabled`](#criticalimagesbeaconenabled), [`CssImageInlineMaxBytes`](#cssimageinlinemaxbytes), [`DocumentImageInlineMaxBytes`](#documentimageinlinemaxbytes), [`EnableLazyLoadHighResImages`](#enablelazyloadhighresimages), [`ImageInlineMaxBytes`](#imageinlinemaxbytes), [`ImageJpegNumProgressiveScans`](#imagejpegnumprogressivescans), [`ImageJpegNumProgressiveScansForSmallScreens`](#imagejpegnumprogressivescansforsmallscreens), [`ImageLimitOptimizedPercent`](#imagelimitoptimizedpercent), [`ImageLimitRenderedAreaPercent`](#imagelimitrenderedareapercent), [`ImageLimitResizeAreaPercent`](#imagelimitresizeareapercent), [`ImageMaxAnimatedFrames`](#imagemaxanimatedframes), [`ImageMaxRewritesAtOnce`](#imagemaxrewritesatonce), [`ImageProvenanceCarry`](#imageprovenancecarry), [`ImageRecompressionQuality`](#imagerecompressionquality), [`ImageResolutionLimitBytes`](#imageresolutionlimitbytes), [`InlineOnlyCriticalImages`](#inlineonlycriticalimages), [`JpegQualityForSaveData`](#jpegqualityforsavedata), [`JpegRecompressionQuality`](#jpegrecompressionquality), [`JpegRecompressionQualityForSmallScreens`](#jpegrecompressionqualityforsmallscreens), [`LazyloadImagesAfterOnload`](#lazyloadimagesafteronload), [`LazyloadImagesBlankUrl`](#lazyloadimagesblankurl), [`LazyloadImagesMode`](#lazyloadimagesmode), [`LazyloadImagesSkipFirst`](#lazyloadimagesskipfirst), [`MaxImageSizeLowResolutionBytes`](#maximagesizelowresolutionbytes), [`MaxInlinedPreviewImagesIndex`](#maxinlinedpreviewimagesindex), [`MaxLowResImageSizeBytes`](#maxlowresimagesizebytes), [`MaxLowResToHighResImageSizePercentage`](#maxlowrestohighresimagesizepercentage), [`MinImageSizeLowResolutionBytes`](#minimagesizelowresolutionbytes), [`PreserveImageProvenance`](#preserveimageprovenance), [`ProgressiveJpegMinBytes`](#progressivejpegminbytes), [`ResponsiveImageDensities`](#responsiveimagedensities), [`ServeRewrittenAvifUrlsToAnyAgent`](#serverewrittenavifurlstoanyagent), [`ServeRewrittenWebpUrlsToAnyAgent`](#serverewrittenwebpurlstoanyagent), [`UseBlankImageForInlinePreview`](#useblankimageforinlinepreview), [`WebpAnimatedRecompressionQuality`](#webpanimatedrecompressionquality), [`WebpQualityForSaveData`](#webpqualityforsavedata), [`WebpRecompressionQuality`](#webprecompressionquality), [`WebpRecompressionQualityForSmallScreens`](#webprecompressionqualityforsmallscreens), [`WebpTimeoutMs`](#webptimeoutms)

#### AvifAnimatedRecompressionQuality {#avifanimatedrecompressionquality}

- **Syntax:** `ModPagespeedAvifAnimatedRecompressionQuality number` (Apache) · `pagespeed AvifAnimatedRecompressionQuality number;` (nginx) · `pagespeed AvifAnimatedRecompressionQuality number` (IIS)
- **Default:** `50`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Quality for rewritten animated avif images [-1,100], 100 refers to best quality, -1 uses ImageRecompressionQuality.

#### AvifQualityForSaveData {#avifqualityforsavedata}

- **Syntax:** `ModPagespeedAvifQualityForSaveData number` (Apache) · `pagespeed AvifQualityForSaveData number;` (nginx) · `pagespeed AvifQualityForSaveData number` (IIS)
- **Default:** `45`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Set quality for the images which will be optimized to lossy AVIF format in the Save-Data mode. Use a value in [0,100] to explicitly set the quality. Use -1 to ignore the Save-Data header.

#### AvifRecompressionQuality {#avifrecompressionquality}

- **Syntax:** `ModPagespeedAvifRecompressionQuality number` (Apache) · `pagespeed AvifRecompressionQuality number;` (nginx) · `pagespeed AvifRecompressionQuality number` (IIS)
- **Default:** `60`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Quality for rewritten avif images [-1,100], 100 refers to best quality, -1 uses ImageRecompressionQuality.

#### AvifRecompressionQualityForSmallScreens {#avifrecompressionqualityforsmallscreens}

- **Syntax:** `ModPagespeedAvifRecompressionQualityForSmallScreens number` (Apache) · `pagespeed AvifRecompressionQualityForSmallScreens number;` (nginx) · `pagespeed AvifRecompressionQualityForSmallScreens number` (IIS)
- **Default:** `50`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Quality for rewritten avif images for small screens. [-1,100], 100 refers to best quality, -1 falls back to AvifRecompressionQuality.

#### AvifTimeoutMs {#aviftimeoutms}

- **Syntax:** `ModPagespeedAvifTimeoutMs number` (Apache) · `pagespeed AvifTimeoutMs number;` (nginx) · `pagespeed AvifTimeoutMs number` (IIS)
- **Default:** `5000`
- **Context:** Apache: server config, virtual host (tolerated) · nginx: http · IIS: pagespeed.config, outside match rules

Timeout for AVIF still-image encoding, in milliseconds; the encoder speed is derived from this budget.

#### CriticalImagesBeaconEnabled {#criticalimagesbeaconenabled}

- **Syntax:** `ModPagespeedCriticalImagesBeaconEnabled on|off` (Apache) · `pagespeed CriticalImagesBeaconEnabled on|off;` (nginx) · `pagespeed CriticalImagesBeaconEnabled on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Enable insertion of client-side critical image detection js for image optimization filters.

#### CssImageInlineMaxBytes {#cssimageinlinemaxbytes}

- **Syntax:** `ModPagespeedCssImageInlineMaxBytes number` (Apache) · `pagespeed CssImageInlineMaxBytes number;` (nginx) · `pagespeed CssImageInlineMaxBytes number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Number of bytes below which CSS images will be inlined.

#### DocumentImageInlineMaxBytes {#documentimageinlinemaxbytes}

- **Syntax:** `ModPagespeedDocumentImageInlineMaxBytes number` (Apache) · `pagespeed DocumentImageInlineMaxBytes number;` (nginx) · `pagespeed DocumentImageInlineMaxBytes number` (IIS)
- **Default:** `4194304`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Most image bytes inlined into one document; further images are left as links.

#### EnableLazyLoadHighResImages {#enablelazyloadhighresimages}

- **Syntax:** `pagespeed EnableLazyLoadHighResImages on|off;` (nginx) · `pagespeed EnableLazyLoadHighResImages on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ImageInlineMaxBytes {#imageinlinemaxbytes}

- **Syntax:** `ModPagespeedImageInlineMaxBytes number` (Apache) · `pagespeed ImageInlineMaxBytes number;` (nginx) · `pagespeed ImageInlineMaxBytes number` (IIS)
- **Default:** `3072`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Number of bytes below which images will be inlined.

#### ImageJpegNumProgressiveScans {#imagejpegnumprogressivescans}

- **Syntax:** `ModPagespeedImageJpegNumProgressiveScans number` (Apache) · `pagespeed ImageJpegNumProgressiveScans number;` (nginx) · `pagespeed ImageJpegNumProgressiveScans number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of progressive scans [1,10] to emit when rewriting images as ten-scan progressive jpegs. A value of -1 outputs all progressive scans.

#### ImageJpegNumProgressiveScansForSmallScreens {#imagejpegnumprogressivescansforsmallscreens}

- **Syntax:** `ModPagespeedImageJpegNumProgressiveScansForSmallScreens number` (Apache) · `pagespeed ImageJpegNumProgressiveScansForSmallScreens number;` (nginx) · `pagespeed ImageJpegNumProgressiveScansForSmallScreens number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of progressive scans [1,10] to emit when rewriting images asten-scan progressive jpegs for small screens. A value of -1 falls back to kImageJpegNumProgressiveScans.

#### ImageLimitOptimizedPercent {#imagelimitoptimizedpercent}

- **Syntax:** `ModPagespeedImageLimitOptimizedPercent number` (Apache) · `pagespeed ImageLimitOptimizedPercent number;` (nginx) · `pagespeed ImageLimitOptimizedPercent number` (IIS)
- **Default:** `100`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Replace images whose size after recompression is less than the given percent of original image size; 100 means replace if smaller.

#### ImageLimitRenderedAreaPercent {#imagelimitrenderedareapercent}

- **Syntax:** `ModPagespeedImageLimitRenderedAreaPercent number` (Apache) · `pagespeed ImageLimitRenderedAreaPercent number;` (nginx) · `pagespeed ImageLimitRenderedAreaPercent number` (IIS)
- **Default:** `95`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Limit on percentage of rendered image wxh to the original image wxh that should be stored in the property cache. This is to avoid corner cases where rounding off decreases the rendered image size by a few pixels.

#### ImageLimitResizeAreaPercent {#imagelimitresizeareapercent}

- **Syntax:** `ModPagespeedImageLimitResizeAreaPercent number` (Apache) · `pagespeed ImageLimitResizeAreaPercent number;` (nginx) · `pagespeed ImageLimitResizeAreaPercent number` (IIS)
- **Default:** `100`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Consider resizing images whose area in pixels is less than the given percent of original image area; 100 means replace if smaller.

#### ImageMaxAnimatedFrames {#imagemaxanimatedframes}

- **Syntax:** `ModPagespeedImageMaxAnimatedFrames number` (Apache) · `pagespeed ImageMaxAnimatedFrames number;` (nginx) · `pagespeed ImageMaxAnimatedFrames number` (IIS)
- **Default:** `200`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Maximum number of frames in an animated image that will be converted. Animations with more frames are served unconverted. 0 or a negative value disables the limit.

#### ImageMaxRewritesAtOnce {#imagemaxrewritesatonce}

- **Syntax:** `ModPagespeedImageMaxRewritesAtOnce number` (Apache) · `pagespeed ImageMaxRewritesAtOnce number;` (nginx) · `pagespeed ImageMaxRewritesAtOnce number` (IIS)
- **Default:** `8`
- **Context:** Apache: server config, virtual host (tolerated) · nginx: http · IIS: pagespeed.config, outside match rules

Set bound on number of images being rewritten at one time (0 = unbounded).

#### ImageProvenanceCarry {#imageprovenancecarry}

- **Syntax:** `ModPagespeedImageProvenanceCarry on|off` (Apache) · `pagespeed ImageProvenanceCarry on|off;` (nginx) · `pagespeed ImageProvenanceCarry on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Provenance carry-through: when true (and PreserveImageProvenance is on), PNG images carrying a C2PA / Content Credentials manifest are recompressed AND their original manifest chunks (caBX/iTXt) are carried into the optimized output unmodified, keeping both the byte savings and the provenance. (Manifest-bearing JPEGs already carry their APP11/JUMBF through recompression under PreserveImageProvenance.) Cases where the carrier cannot be preserved byte-exactly -- resize, format conversion (PNG-&gt;JPEG/WebP), GIF, or extraction failure -- fall back to the default detect-and-skip pass-through. No effect when PreserveImageProvenance is false.

#### ImageRecompressionQuality {#imagerecompressionquality}

- **Syntax:** `ModPagespeedImageRecompressionQuality number` (Apache) · `pagespeed ImageRecompressionQuality number;` (nginx) · `pagespeed ImageRecompressionQuality number` (IIS)
- **Default:** `85`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Set quality parameter for recompressing images [-1,100], 100 refers to best quality, -1 disables lossy compression. JpegRecompressionQuality and WebpRecompressionQuality override this.

#### ImageResolutionLimitBytes {#imageresolutionlimitbytes}

- **Syntax:** `ModPagespeedImageResolutionLimitBytes number` (Apache) · `pagespeed ImageResolutionLimitBytes number;` (nginx) · `pagespeed ImageResolutionLimitBytes number` (IIS)
- **Default:** `33554432`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Maximum byte size of an image for optimization

#### InlineOnlyCriticalImages {#inlineonlycriticalimages}

- **Syntax:** `ModPagespeedInlineOnlyCriticalImages on|off` (Apache) · `pagespeed InlineOnlyCriticalImages on|off;` (nginx) · `pagespeed InlineOnlyCriticalImages on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Inline only critical images

#### JpegQualityForSaveData {#jpegqualityforsavedata}

- **Syntax:** `ModPagespeedJpegQualityForSaveData number` (Apache) · `pagespeed JpegQualityForSaveData number;` (nginx) · `pagespeed JpegQualityForSaveData number` (IIS)
- **Default:** `50`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Set quality for the images which will be optimized to JPEG format in the Save-Data mode. Use a value in [0,100] to explicitly set the quality. Use -1 to ignore the Save-Data header.

#### JpegRecompressionQuality {#jpegrecompressionquality}

- **Syntax:** `ModPagespeedJpegRecompressionQuality number` (Apache) · `pagespeed JpegRecompressionQuality number;` (nginx) · `pagespeed JpegRecompressionQuality number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Set quality parameter for recompressing jpeg images [-1,100], 100 is lossless, -1 uses ImageRecompressionQuality

#### JpegRecompressionQualityForSmallScreens {#jpegrecompressionqualityforsmallscreens}

- **Syntax:** `ModPagespeedJpegRecompressionQualityForSmallScreens number` (Apache) · `pagespeed JpegRecompressionQualityForSmallScreens number;` (nginx) · `pagespeed JpegRecompressionQualityForSmallScreens number` (IIS)
- **Default:** `70`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Set quality parameter for recompressing jpeg images for small screens. [-1,100], 100 refers to best quality, -1 falls back to ImageJpegRecompressionQuality.

#### LazyloadImagesAfterOnload {#lazyloadimagesafteronload}

- **Syntax:** `ModPagespeedLazyloadImagesAfterOnload on|off` (Apache) · `pagespeed LazyloadImagesAfterOnload on|off;` (nginx) · `pagespeed LazyloadImagesAfterOnload on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Wait until page onload before loading lazy images

#### LazyloadImagesBlankUrl {#lazyloadimagesblankurl}

- **Syntax:** `ModPagespeedLazyloadImagesBlankUrl value` (Apache) · `pagespeed LazyloadImagesBlankUrl value;` (nginx) · `pagespeed LazyloadImagesBlankUrl value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

URL of image used to display prior to loading the lazy image. Empty means use a site-local copy.

#### LazyloadImagesMode {#lazyloadimagesmode}

- **Syntax:** `ModPagespeedLazyloadImagesMode auto|native|js` (Apache) · `pagespeed LazyloadImagesMode auto|native|js;` (nginx) · `pagespeed LazyloadImagesMode auto|native|js` (IIS)
- **Default:** `auto`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

How to lazyload images: 'native' annotates non-critical images with loading="lazy", 'js' uses the JavaScript loader, and 'auto' (the default) picks per user agent.

#### LazyloadImagesSkipFirst {#lazyloadimagesskipfirst}

- **Syntax:** `ModPagespeedLazyloadImagesSkipFirst number` (Apache) · `pagespeed LazyloadImagesSkipFirst number;` (nginx) · `pagespeed LazyloadImagesSkipFirst number` (IIS)
- **Default:** `1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of leading images to leave untouched when critical image data is unavailable, protecting likely LCP candidates.

#### MaxImageSizeLowResolutionBytes {#maximagesizelowresolutionbytes}

- **Syntax:** `ModPagespeedMaxImageSizeLowResolutionBytes number` (Apache) · `pagespeed MaxImageSizeLowResolutionBytes number;` (nginx) · `pagespeed MaxImageSizeLowResolutionBytes number` (IIS)
- **Default:** `1048576`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Maximum image size below which low resolution image is generated.

#### MaxInlinedPreviewImagesIndex {#maxinlinedpreviewimagesindex}

- **Syntax:** `ModPagespeedMaxInlinedPreviewImagesIndex number` (Apache) · `pagespeed MaxInlinedPreviewImagesIndex number;` (nginx) · `pagespeed MaxInlinedPreviewImagesIndex number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of first N images for which low resolution image is generated. Negative values result in generation for all images.

#### MaxLowResImageSizeBytes {#maxlowresimagesizebytes}

- **Syntax:** `pagespeed MaxLowResImageSizeBytes number;` (nginx) · `pagespeed MaxLowResImageSizeBytes number` (IIS)
- **Default:** `-1`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### MaxLowResToHighResImageSizePercentage {#maxlowrestohighresimagesizepercentage}

- **Syntax:** `pagespeed MaxLowResToHighResImageSizePercentage number;` (nginx) · `pagespeed MaxLowResToHighResImageSizePercentage number` (IIS)
- **Default:** `100`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### MinImageSizeLowResolutionBytes {#minimagesizelowresolutionbytes}

- **Syntax:** `ModPagespeedMinImageSizeLowResolutionBytes number` (Apache) · `pagespeed MinImageSizeLowResolutionBytes number;` (nginx) · `pagespeed MinImageSizeLowResolutionBytes number` (IIS)
- **Default:** `3072`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Minimum image size above which low resolution image is generated.

#### PreserveImageProvenance {#preserveimageprovenance}

- **Syntax:** `ModPagespeedPreserveImageProvenance on|off` (Apache) · `pagespeed PreserveImageProvenance on|off;` (nginx) · `pagespeed PreserveImageProvenance on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Preserve C2PA/Content-Credentials provenance metadata (JPEG APP11/JUMBF) through image optimization. On by default; independent of EXIF stripping.

#### ProgressiveJpegMinBytes {#progressivejpegminbytes}

- **Syntax:** `ModPagespeedProgressiveJpegMinBytes number` (Apache) · `pagespeed ProgressiveJpegMinBytes number;` (nginx) · `pagespeed ProgressiveJpegMinBytes number` (IIS)
- **Default:** `10240`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Minimum size in bytes for converting a jpeg to progressive

#### ResponsiveImageDensities {#responsiveimagedensities}

- **Syntax:** `ModPagespeedResponsiveImageDensities value` (Apache) · `pagespeed ResponsiveImageDensities value;` (nginx) · `pagespeed ResponsiveImageDensities value` (IIS)
- **Default:** `1.5,2,3`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Comma separated list of screen densities to target with ResponsiveImageFilter srcsets.

#### ServeRewrittenAvifUrlsToAnyAgent {#serverewrittenavifurlstoanyagent}

- **Syntax:** `ModPagespeedServeRewrittenAvifUrlsToAnyAgent on|off` (Apache) · `pagespeed ServeRewrittenAvifUrlsToAnyAgent on|off;` (nginx) · `pagespeed ServeRewrittenAvifUrlsToAnyAgent on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Serve rewritten .avif images to any user-agent

#### ServeRewrittenWebpUrlsToAnyAgent {#serverewrittenwebpurlstoanyagent}

- **Syntax:** `ModPagespeedServeRewrittenWebpUrlsToAnyAgent on|off` (Apache) · `pagespeed ServeRewrittenWebpUrlsToAnyAgent on|off;` (nginx) · `pagespeed ServeRewrittenWebpUrlsToAnyAgent on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Serve rewritten .webp images to any user-agent

#### UseBlankImageForInlinePreview {#useblankimageforinlinepreview}

- **Syntax:** `ModPagespeedUseBlankImageForInlinePreview on|off` (Apache) · `pagespeed UseBlankImageForInlinePreview on|off;` (nginx) · `pagespeed UseBlankImageForInlinePreview on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Use a blank image for inline preview

#### WebpAnimatedRecompressionQuality {#webpanimatedrecompressionquality}

- **Syntax:** `ModPagespeedWebpAnimatedRecompressionQuality number` (Apache) · `pagespeed WebpAnimatedRecompressionQuality number;` (nginx) · `pagespeed WebpAnimatedRecompressionQuality number` (IIS)
- **Default:** `70`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Quality for rewritten animated webp images [-1,100], 100 refers to best quality, -1 uses ImageRecompressionQuality.

#### WebpQualityForSaveData {#webpqualityforsavedata}

- **Syntax:** `ModPagespeedWebpQualityForSaveData number` (Apache) · `pagespeed WebpQualityForSaveData number;` (nginx) · `pagespeed WebpQualityForSaveData number` (IIS)
- **Default:** `50`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Set quality for the images which will be optimized to lossy WebP format in the Save-Data mode. Use a value in [0,100] to explicitly set the quality. Use -1 to ignore the Save-Data header.

#### WebpRecompressionQuality {#webprecompressionquality}

- **Syntax:** `ModPagespeedWebpRecompressionQuality number` (Apache) · `pagespeed WebpRecompressionQuality number;` (nginx) · `pagespeed WebpRecompressionQuality number` (IIS)
- **Default:** `80`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Quality for rewritten webp images [-1,100], 100 refers to best quality, -1 uses ImageRecompressionQuality.

#### WebpRecompressionQualityForSmallScreens {#webprecompressionqualityforsmallscreens}

- **Syntax:** `ModPagespeedWebpRecompressionQualityForSmallScreens number` (Apache) · `pagespeed WebpRecompressionQualityForSmallScreens number;` (nginx) · `pagespeed WebpRecompressionQualityForSmallScreens number` (IIS)
- **Default:** `70`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Quality for rewritten webp images for small screens. [-1,100], 100 refers to best quality, -1 falls back to WebpRecompressionQuality.

#### WebpTimeoutMs {#webptimeoutms}

- **Syntax:** `pagespeed WebpTimeoutMs number;` (nginx) · `pagespeed WebpTimeoutMs number` (IIS)
- **Default:** `-1`
- **Context:** nginx: http · IIS: pagespeed.config, outside match rules

_The module source carries no help text for this option._

### CSS {#area-css}

[`AlwaysRewriteCss`](#alwaysrewritecss), [`CriticalCssAboveTheFoldOnly`](#criticalcssabovethefoldonly), [`CssFlattenMaxBytes`](#cssflattenmaxbytes), [`CssInlineMaxBytes`](#cssinlinemaxbytes), [`CssOutlineMinBytes`](#cssoutlineminbytes), [`CssParseMaxBytes`](#cssparsemaxbytes), [`GoogleFontCssInlineMaxBytes`](#googlefontcssinlinemaxbytes), [`MaxCombinedCssBytes`](#maxcombinedcssbytes), [`PermitIdsForCssCombining`](#permitidsforcsscombining), [`TestOnlyPrioritizeCriticalCssDontApplyOriginalCss`](#testonlyprioritizecriticalcssdontapplyoriginalcss)

#### AlwaysRewriteCss {#alwaysrewritecss}

- **Syntax:** `pagespeed AlwaysRewriteCss on|off;` (nginx) · `pagespeed AlwaysRewriteCss on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### CriticalCssAboveTheFoldOnly {#criticalcssabovethefoldonly}

- **Syntax:** `ModPagespeedCriticalCssAboveTheFoldOnly on|off` (Apache) · `pagespeed CriticalCssAboveTheFoldOnly on|off;` (nginx) · `pagespeed CriticalCssAboveTheFoldOnly on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Makes prioritize_critical_css inline only the rules for content in the first screen instead of the rules for the whole page. The inline block is smaller; content further down may be shown partly styled until the full stylesheet has arrived.

#### CssFlattenMaxBytes {#cssflattenmaxbytes}

- **Syntax:** `ModPagespeedCssFlattenMaxBytes number` (Apache) · `pagespeed CssFlattenMaxBytes number;` (nginx) · `pagespeed CssFlattenMaxBytes number` (IIS)
- **Default:** `1024000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of bytes below which stylesheets will be flattened.

Set it in the server or directory configuration. From 2.8.0 it can no longer
be set on a single request: a `PageSpeedCssFlattenMaxBytes` query parameter,
request or response header, or cookie has no effect. In 2.7.0 and earlier it
could also be set per request.

#### CssInlineMaxBytes {#cssinlinemaxbytes}

- **Syntax:** `ModPagespeedCssInlineMaxBytes number` (Apache) · `pagespeed CssInlineMaxBytes number;` (nginx) · `pagespeed CssInlineMaxBytes number` (IIS)
- **Default:** `2048`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Number of bytes below which stylesheets will be inlined.

#### CssOutlineMinBytes {#cssoutlineminbytes}

- **Syntax:** `ModPagespeedCssOutlineMinBytes number` (Apache) · `pagespeed CssOutlineMinBytes number;` (nginx) · `pagespeed CssOutlineMinBytes number` (IIS)
- **Default:** `3000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of bytes above which inline CSS resources will be outlined.

#### CssParseMaxBytes {#cssparsemaxbytes}

- **Syntax:** `ModPagespeedCssParseMaxBytes number` (Apache) · `pagespeed CssParseMaxBytes number;` (nginx) · `pagespeed CssParseMaxBytes number` (IIS)
- **Default:** `2097152`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Largest stylesheet, in bytes, that the CSS rewriter will parse; larger ones are passed through unmodified (&lt;= 0 for no limit).

#### GoogleFontCssInlineMaxBytes {#googlefontcssinlinemaxbytes}

- **Syntax:** `ModPagespeedGoogleFontCssInlineMaxBytes number` (Apache) · `pagespeed GoogleFontCssInlineMaxBytes number;` (nginx) · `pagespeed GoogleFontCssInlineMaxBytes number` (IIS)
- **Default:** `49152`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Number of bytes below which Google Font stylesheets will be inlined.

#### MaxCombinedCssBytes {#maxcombinedcssbytes}

- **Syntax:** `ModPagespeedMaxCombinedCssBytes number` (Apache) · `pagespeed MaxCombinedCssBytes number;` (nginx) · `pagespeed MaxCombinedCssBytes number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Maximum size allowed for the combined CSS resource.

#### PermitIdsForCssCombining {#permitidsforcsscombining}

- **Syntax:** `ModPagespeedPermitIdsForCssCombining wildcard` (Apache) · `pagespeed PermitIdsForCssCombining wildcard;` (nginx) · `pagespeed PermitIdsForCssCombining wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Allow combining CSS files with IDs matching wildcard

#### TestOnlyPrioritizeCriticalCssDontApplyOriginalCss {#testonlyprioritizecriticalcssdontapplyoriginalcss}

- **Syntax:** `ModPagespeedTestOnlyPrioritizeCriticalCssDontApplyOriginalCss on|off` (Apache) · `pagespeed TestOnlyPrioritizeCriticalCssDontApplyOriginalCss on|off;` (nginx) · `pagespeed TestOnlyPrioritizeCriticalCssDontApplyOriginalCss on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Stops the prioritize_critical_css filter from invoking its JavaScript that applies all the 'hidden' CSS at onload. Intended for testing.

### JavaScript {#area-javascript}

[`AvoidRenamingIntrospectiveJavascript`](#avoidrenamingintrospectivejavascript), [`ClearInheritedScripts`](#clearinheritedscripts), [`EnableDeferJsExperimental`](#enabledeferjsexperimental), [`EnablePrioritizingScripts`](#enableprioritizingscripts), [`JsInlineMaxBytes`](#jsinlinemaxbytes), [`JsOutlineMinBytes`](#jsoutlineminbytes), [`Library`](#library), [`MaxCombinedJsBytes`](#maxcombinedjsbytes), [`ProcessScriptVariables`](#processscriptvariables)

#### AvoidRenamingIntrospectiveJavascript {#avoidrenamingintrospectivejavascript}

- **Syntax:** `ModPagespeedAvoidRenamingIntrospectiveJavascript on|off` (Apache) · `pagespeed AvoidRenamingIntrospectiveJavascript on|off;` (nginx) · `pagespeed AvoidRenamingIntrospectiveJavascript on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Don't combine, inline, cache extend, or otherwise modify javascript in ways that require changing the URL if we see introspection in the form of document.getElementsByTagName('script').

#### ClearInheritedScripts {#clearinheritedscripts}

- **Syntax:** `pagespeed ClearInheritedScripts;` (nginx)
- **Default:** —
- **Context:** nginx: http, server, location
- **Platform:** nginx only

Drops the script-variable directive lines inherited from the enclosing block, so this block starts from the non-scripted configuration.

#### EnableDeferJsExperimental {#enabledeferjsexperimental}

- **Syntax:** `ModPagespeedEnableDeferJsExperimental on|off` (Apache) · `pagespeed EnableDeferJsExperimental on|off;` (nginx) · `pagespeed EnableDeferJsExperimental on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Enable experimental options in defer javascript.

#### EnablePrioritizingScripts {#enableprioritizingscripts}

- **Syntax:** `pagespeed EnablePrioritizingScripts on|off;` (nginx) · `pagespeed EnablePrioritizingScripts on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### JsInlineMaxBytes {#jsinlinemaxbytes}

- **Syntax:** `ModPagespeedJsInlineMaxBytes number` (Apache) · `pagespeed JsInlineMaxBytes number;` (nginx) · `pagespeed JsInlineMaxBytes number` (IIS)
- **Default:** `2048`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Number of bytes below which javascript will be inlined.

#### JsOutlineMinBytes {#jsoutlineminbytes}

- **Syntax:** `ModPagespeedJsOutlineMinBytes number` (Apache) · `pagespeed JsOutlineMinBytes number;` (nginx) · `pagespeed JsOutlineMinBytes number` (IIS)
- **Default:** `3000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of bytes above which inline Javascript resources willbe outlined.

#### Library {#library}

- **Syntax:** `ModPagespeedLibrary bytes md5 canonical_url` (Apache) · `pagespeed Library bytes md5 canonical_url;` (nginx) · `pagespeed Library bytes md5 canonical_url` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Recognizes a JavaScript library by size and hash so canonicalize_javascript_libraries can replace it with the given canonical URL.

#### MaxCombinedJsBytes {#maxcombinedjsbytes}

- **Syntax:** `ModPagespeedMaxCombinedJsBytes number` (Apache) · `pagespeed MaxCombinedJsBytes number;` (nginx) · `pagespeed MaxCombinedJsBytes number` (IIS)
- **Default:** `92160`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Maximum size allowed for the combined JavaScript resource.

#### ProcessScriptVariables {#processscriptvariables}

- **Syntax:** `pagespeed ProcessScriptVariables on|off|all;` (nginx)
- **Default:** —
- **Context:** nginx: http
- **Platform:** nginx only

Evaluate nginx script variables ($var) in pagespeed directive arguments at request time: off, on (the LoadFromFile\*, EnableFilters, DisableFilters, DownstreamCache\* and ShardDomain directives) or all (every query- and directory-scoped option too). Settable once, at the top level.

### Caching {#area-caching}

[`AsyncMetadataL2Writes`](#asyncmetadatal2writes), [`AutoCreateCachePath`](#autocreatecachepath), [`AwaitPcacheLookup`](#awaitpcachelookup), [`CacheFlushFilename`](#cacheflushfilename), [`CacheFlushPollIntervalSec`](#cacheflushpollintervalsec), [`CacheFragment`](#cachefragment), [`CompressMetadataCache`](#compressmetadatacache), [`CreateSharedMemoryMetadataCache`](#createsharedmemorymetadatacache), [`CycloneRamCacheKb`](#cycloneramcachekb), [`CycloneZeroCopy`](#cyclonezerocopy), [`CycloneZeroCopyServe`](#cyclonezerocopyserve), [`DefaultCacheHtml`](#defaultcachehtml), [`DefaultSharedMemoryCacheKB`](#defaultsharedmemorycachekb), [`DownstreamCachePurgeLocationPrefix`](#downstreamcachepurgelocationprefix), [`DownstreamCachePurgeMethod`](#downstreamcachepurgemethod), [`DownstreamCacheRebeaconingKey`](#downstreamcacherebeaconingkey), [`DownstreamCacheRewrittenPercentageThreshold`](#downstreamcacherewrittenpercentagethreshold), [`EnableCachePurge`](#enablecachepurge), [`FileCachePath`](#filecachepath), [`FileCacheSizeKb`](#filecachesizekb), [`FileCacheSmallTierPercent`](#filecachesmalltierpercent), [`FinderPropertiesCacheExpirationTimeMs`](#finderpropertiescacheexpirationtimems), [`FinderPropertiesCacheRefreshTimeMs`](#finderpropertiescacherefreshtimems), [`ForceCaching`](#forcecaching), [`HttpCacheCompressionLevel`](#httpcachecompressionlevel), [`ImplicitCacheTtlMs`](#implicitcachettlms), [`LRUCacheByteLimit`](#lrucachebytelimit), [`LRUCacheKbPerProcess`](#lrucachekbperprocess), [`MaxCacheableContentLength`](#maxcacheablecontentlength), [`MaxHtmlCacheTimeMs`](#maxhtmlcachetimems), [`MemcachedServers`](#memcachedservers), [`MemcachedThreads`](#memcachedthreads), [`MemcachedTimeoutUs`](#memcachedtimeoutus), [`MetadataCacheStalenessThresholdMs`](#metadatacachestalenessthresholdms), [`MinResourceCacheTimeToRewriteMs`](#minresourcecachetimetorewritems), [`NonCacheablesForCachePartialHtml`](#noncacheablesforcachepartialhtml), [`OverrideCachingTtlMs`](#overridecachingttlms), [`PubliclyCacheMismatchedHashesExperimental`](#publiclycachemismatchedhashesexperimental), [`PurgeMethod`](#purgemethod), [`RedisDatabaseIndex`](#redisdatabaseindex), [`RedisPassword`](#redispassword), [`RedisReconnectionDelayMs`](#redisreconnectiondelayms), [`RedisServer`](#redisserver), [`RedisTimeoutUs`](#redistimeoutus), [`RedisTTLSec`](#redisttlsec), [`RedisUser`](#redisuser), [`SharedMemoryLocks`](#sharedmemorylocks), [`ShmMetadataCacheCheckpointIntervalSec`](#shmmetadatacachecheckpointintervalsec), [`UseFallbackPropertyCacheValues`](#usefallbackpropertycachevalues)

#### AsyncMetadataL2Writes {#asyncmetadatal2writes}

- **Syntax:** `ModPagespeedAsyncMetadataL2Writes on|off` (Apache) · `pagespeed AsyncMetadataL2Writes on|off;` (nginx) · `pagespeed AsyncMetadataL2Writes on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Defer the metadata cache's blocking L2 (disk) write off the rewrite critical path onto a single-thread write-behind queue. Reads and the shared-memory L1 write stay synchronous, so same-machine read-your-writes is preserved. Experimental; off by default.

#### AutoCreateCachePath {#autocreatecachepath}

- **Syntax:** `pagespeed AutoCreateCachePath on|off` (IIS)
- **Default:** `on`
- **Context:** IIS: pagespeed.config, outside match rules
- **Platform:** IIS only

Auto-create the per-site cache subdirectory if missing (on|off, default on).

Creation applies only to a cache directory inside
`C:\ProgramData\We-Amp\PageSpeed\cache\` or
`C:\ProgramData\We-Amp\IISWebSpeed\cache\`; a cache directory anywhere else
must already exist when the site serves its first request, whatever this is set
to. With `off`, a missing directory is not created and the site does not start
optimizing. See
[IIS configuration](/docs/iis-configuration/#automatic-cache-directory-creation).

#### AwaitPcacheLookup {#awaitpcachelookup}

- **Syntax:** `pagespeed AwaitPcacheLookup on|off;` (nginx) · `pagespeed AwaitPcacheLookup on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server · IIS: pagespeed.config, outside match rules

_The module source carries no help text for this option._

#### CacheFlushFilename {#cacheflushfilename}

- **Syntax:** `ModPagespeedCacheFlushFilename value` (Apache) · `pagespeed CacheFlushFilename value;` (nginx) · `pagespeed CacheFlushFilename value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Name of file to check for timestamp updates used to flush cache. This file will be relative to the ModPagespeedFileCachePath if it does not begin with a slash.

#### CacheFlushPollIntervalSec {#cacheflushpollintervalsec}

- **Syntax:** `ModPagespeedCacheFlushPollIntervalSec number` (Apache) · `pagespeed CacheFlushPollIntervalSec number;` (nginx) · `pagespeed CacheFlushPollIntervalSec number` (IIS)
- **Default:** `5`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Number of seconds to wait between polling for cache-flush requests

#### CacheFragment {#cachefragment}

- **Syntax:** `ModPagespeedCacheFragment value` (Apache) · `pagespeed CacheFragment value;` (nginx) · `pagespeed CacheFragment value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Set a cache fragment to allow servers with different hostnames to share a cache. Allowed: letters, numbers, underscores, and hyphens.

#### CompressMetadataCache {#compressmetadatacache}

- **Syntax:** `ModPagespeedCompressMetadataCache on|off` (Apache) · `pagespeed CompressMetadataCache on|off;` (nginx) · `pagespeed CompressMetadataCache on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Whether to compress cache entries before writing them to memory or disk.

#### CreateSharedMemoryMetadataCache {#createsharedmemorymetadatacache}

- **Syntax:** `ModPagespeedCreateSharedMemoryMetadataCache name size_kb` (Apache) · `pagespeed CreateSharedMemoryMetadataCache name size_kb;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Creates a named shared-memory metadata cache of the given size for the virtual hosts whose FileCachePath is that name.

#### CycloneRamCacheKb {#cycloneramcachekb}

- **Syntax:** `ModPagespeedCycloneRamCacheKb number` (Apache) · `pagespeed CycloneRamCacheKb number;` (nginx) · `pagespeed CycloneRamCacheKb number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the size, in KB, of Cyclone's internal RAM cache tier, decoupled from LRUCacheKbPerProcess. 0 (the default) disables the RAM tier -- reads are served from the memory-mapped volume, which the OS page cache already keeps hot; -1 inherits LRUCacheKbPerProcess.

#### CycloneZeroCopy {#cyclonezerocopy}

- **Syntax:** `ModPagespeedCycloneZeroCopy on|off` (Apache) · `pagespeed CycloneZeroCopy on|off;` (nginx) · `pagespeed CycloneZeroCopy on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Serve HTTP cache hits directly from the Cyclone cache's memory-mapped storage without copying the payload (zero-copy). Off by default on every port; set it on explicitly to opt in.

#### CycloneZeroCopyServe {#cyclonezerocopyserve}

- **Syntax:** `ModPagespeedCycloneZeroCopyServe on|off` (Apache) · `pagespeed CycloneZeroCopyServe on|off;` (nginx) · `pagespeed CycloneZeroCopyServe on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Carry memory-mapped Cyclone cache-hit bytes into the port output buffer by reference (aliased) instead of copying them; the serve safely copies the tail out before a cache eviction can overwrite it. Engages only once CycloneZeroCopy maps cache values; on Apache the aliased serve is experimental and activates only when this option is explicitly set. Apache aliases only plain-HTTP/1.x main-request 200s of at least 16KB served verbatim (no Range, no deflate/ssl/http2 or other transforming filter), and bytes an output filter parks for a slow client are copied out at that point; everything else serves a verified copy.

#### DefaultCacheHtml {#defaultcachehtml}

- **Syntax:** `pagespeed DefaultCacheHtml on|off;` (nginx) · `pagespeed DefaultCacheHtml on|off` (IIS)
- **Default:** `off`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### DefaultSharedMemoryCacheKB {#defaultsharedmemorycachekb}

- **Syntax:** `ModPagespeedDefaultSharedMemoryCacheKB number` (Apache) · `pagespeed DefaultSharedMemoryCacheKB number;` (nginx) · `pagespeed DefaultSharedMemoryCacheKB number` (IIS)
- **Default:** `51200`
- **Context:** Apache: server config, virtual host (tolerated) · nginx: http · IIS: pagespeed.config, outside match rules

Size of the default shared memory cache used by all virtual hosts that don't use CreateSharedMemoryMetadataCache. Set to 0 to turn off the default shared memory cache.

#### DownstreamCachePurgeLocationPrefix {#downstreamcachepurgelocationprefix}

- **Syntax:** `ModPagespeedDownstreamCachePurgeLocationPrefix host:port/path` (Apache) · `pagespeed DownstreamCachePurgeLocationPrefix host:port/path;` (nginx) · `pagespeed DownstreamCachePurgeLocationPrefix host:port/path` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

The host:port/path prefix to be used for purging requests from the downstream cache.

#### DownstreamCachePurgeMethod {#downstreamcachepurgemethod}

- **Syntax:** `ModPagespeedDownstreamCachePurgeMethod value` (Apache) · `pagespeed DownstreamCachePurgeMethod value;` (nginx) · `pagespeed DownstreamCachePurgeMethod value` (IIS)
- **Default:** `PURGE`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Method to be used for purging responses from the downstream cache

#### DownstreamCacheRebeaconingKey {#downstreamcacherebeaconingkey}

- **Syntax:** `ModPagespeedDownstreamCacheRebeaconingKey value` (Apache) · `pagespeed DownstreamCacheRebeaconingKey value;` (nginx) · `pagespeed DownstreamCacheRebeaconingKey value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

The key used to authenticate rebeaconing requests from downstream caches. The value specified for this key in the pagespeed server config should be used in the caching layer configuration also.

#### DownstreamCacheRewrittenPercentageThreshold {#downstreamcacherewrittenpercentagethreshold}

- **Syntax:** `ModPagespeedDownstreamCacheRewrittenPercentageThreshold number` (Apache) · `pagespeed DownstreamCacheRewrittenPercentageThreshold number;` (nginx) · `pagespeed DownstreamCacheRewrittenPercentageThreshold number` (IIS)
- **Default:** `95`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Threshold for percentage of rewriting to be finished before the response is served out and simultaneously stored in the downstream cache, beyond which the response will not be purged from the cache evenif more rewriting is possible now

#### EnableCachePurge {#enablecachepurge}

- **Syntax:** `ModPagespeedEnableCachePurge on|off` (Apache) · `pagespeed EnableCachePurge on|off;` (nginx) · `pagespeed EnableCachePurge on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Allows individual resources to be flushed; adding some overhead to the metadata cache

#### FileCachePath {#filecachepath}

- **Syntax:** `ModPagespeedFileCachePath value` (Apache) · `pagespeed FileCachePath value;` (nginx) · `pagespeed FileCachePath value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the path for file cache

#### FileCacheSizeKb {#filecachesizekb}

- **Syntax:** `ModPagespeedFileCacheSizeKb number` (Apache) · `pagespeed FileCacheSizeKb number;` (nginx) · `pagespeed FileCacheSizeKb number` (IIS)
- **Default:** `1048576`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the target size (in kilobytes) for file cache

#### FileCacheSmallTierPercent {#filecachesmalltierpercent}

- **Syntax:** `ModPagespeedFileCacheSmallTierPercent number` (Apache) · `pagespeed FileCacheSmallTierPercent number;` (nginx) · `pagespeed FileCacheSmallTierPercent number` (IIS)
- **Default:** `10`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Percentage of the file cache carved out as a separate small-object volume that protects metadata and property entries from payload churn. 0 disables the tier; values are clamped to [0, 50]. Below roughly 256 MB of total file cache the tier disables itself and entries share the main volume. Applies in the default shared-memory metadata cache configuration; with the shm metadata cache disabled, metadata stays on the main volume.

#### FinderPropertiesCacheExpirationTimeMs {#finderpropertiescacheexpirationtimems}

- **Syntax:** `ModPagespeedFinderPropertiesCacheExpirationTimeMs number` (Apache) · `pagespeed FinderPropertiesCacheExpirationTimeMs number;` (nginx) · `pagespeed FinderPropertiesCacheExpirationTimeMs number` (IIS)
- **Default:** `7200000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Number of ms that beacon results for the critical selector finders should be considered valid.

#### FinderPropertiesCacheRefreshTimeMs {#finderpropertiescacherefreshtimems}

- **Syntax:** `pagespeed FinderPropertiesCacheRefreshTimeMs number;` (nginx) · `pagespeed FinderPropertiesCacheRefreshTimeMs number` (IIS)
- **Default:** `5400000`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ForceCaching {#forcecaching}

- **Syntax:** `ModPagespeedForceCaching on|off` (Apache) · `pagespeed ForceCaching on|off;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Ignore HTTP cache headers and TTLs

#### HttpCacheCompressionLevel {#httpcachecompressionlevel}

- **Syntax:** `ModPagespeedHttpCacheCompressionLevel number` (Apache) · `pagespeed HttpCacheCompressionLevel number;` (nginx) · `pagespeed HttpCacheCompressionLevel number` (IIS)
- **Default:** `9`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Compression level for HTTPCache. [-1-9] where 0 is off, 1 is minimumcompression, and 9 (the default) is maximum compression.

#### ImplicitCacheTtlMs {#implicitcachettlms}

- **Syntax:** `ModPagespeedImplicitCacheTtlMs number` (Apache) · `pagespeed ImplicitCacheTtlMs number;` (nginx) · `pagespeed ImplicitCacheTtlMs number` (IIS)
- **Default:** `300000`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Time in milliseconds to cache resources that lack an Expires or Cache-Control header

#### LRUCacheByteLimit {#lrucachebytelimit}

- **Syntax:** `ModPagespeedLRUCacheByteLimit number` (Apache) · `pagespeed LRUCacheByteLimit number;` (nginx) · `pagespeed LRUCacheByteLimit number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the maximum byte size entry to store in the per-process in-memory LRU cache

#### LRUCacheKbPerProcess {#lrucachekbperprocess}

- **Syntax:** `ModPagespeedLRUCacheKbPerProcess number` (Apache) · `pagespeed LRUCacheKbPerProcess number;` (nginx) · `pagespeed LRUCacheKbPerProcess number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the total size, in KB, of the per-process in-memory LRU cache

#### MaxCacheableContentLength {#maxcacheablecontentlength}

- **Syntax:** `ModPagespeedMaxCacheableContentLength number` (Apache) · `pagespeed MaxCacheableContentLength number;` (nginx) · `pagespeed MaxCacheableContentLength number` (IIS)
- **Default:** `16777216`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Maximum length of a cacheable response content. To remove this limit, use -1.

#### MaxHtmlCacheTimeMs {#maxhtmlcachetimems}

- **Syntax:** `pagespeed MaxHtmlCacheTimeMs number;` (nginx) · `pagespeed MaxHtmlCacheTimeMs number` (IIS)
- **Default:** `0`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### MemcachedServers {#memcachedservers}

- **Syntax:** `ModPagespeedMemcachedServers value` (Apache) · `pagespeed MemcachedServers value;` (nginx) · `pagespeed MemcachedServers value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Comma-separated list of servers e.g. host1:port1,host2:port2

#### MemcachedThreads {#memcachedthreads}

- **Syntax:** `ModPagespeedMemcachedThreads number` (Apache) · `pagespeed MemcachedThreads number;` (nginx) · `pagespeed MemcachedThreads number` (IIS)
- **Default:** `1`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Number of background threads to use to run memcached fetches

#### MemcachedTimeoutUs {#memcachedtimeoutus}

- **Syntax:** `ModPagespeedMemcachedTimeoutUs number` (Apache) · `pagespeed MemcachedTimeoutUs number;` (nginx) · `pagespeed MemcachedTimeoutUs number` (IIS)
- **Default:** `500000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Maximum time in microseconds to allow for memcached transactions

#### MetadataCacheStalenessThresholdMs {#metadatacachestalenessthresholdms}

- **Syntax:** `pagespeed MetadataCacheStalenessThresholdMs number;` (nginx) · `pagespeed MetadataCacheStalenessThresholdMs number` (IIS)
- **Default:** `0`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### MinResourceCacheTimeToRewriteMs {#minresourcecachetimetorewritems}

- **Syntax:** `pagespeed MinResourceCacheTimeToRewriteMs number;` (nginx) · `pagespeed MinResourceCacheTimeToRewriteMs number` (IIS)
- **Default:** `0`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### NonCacheablesForCachePartialHtml {#noncacheablesforcachepartialhtml}

- **Syntax:** `pagespeed NonCacheablesForCachePartialHtml value;` (nginx) · `pagespeed NonCacheablesForCachePartialHtml value` (IIS)
- **Default:** `(empty)`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### OverrideCachingTtlMs {#overridecachingttlms}

- **Syntax:** `pagespeed OverrideCachingTtlMs number;` (nginx) · `pagespeed OverrideCachingTtlMs number` (IIS)
- **Default:** `-1`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### PubliclyCacheMismatchedHashesExperimental {#publiclycachemismatchedhashesexperimental}

- **Syntax:** `ModPagespeedPubliclyCacheMismatchedHashesExperimental on|off` (Apache) · `pagespeed PubliclyCacheMismatchedHashesExperimental on|off;` (nginx) · `pagespeed PubliclyCacheMismatchedHashesExperimental on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

When serving a request for a .pagespeed. URL with the wrong hash, allow public caching based on the origin TTL.

#### PurgeMethod {#purgemethod}

- **Syntax:** `ModPagespeedPurgeMethod value` (Apache) · `pagespeed PurgeMethod value;` (nginx) · `pagespeed PurgeMethod value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

HTTP method used for Cache Purge requests. Typically this is set to PURGE, but you must ensure that only authorized clients have access to this method.

#### RedisDatabaseIndex {#redisdatabaseindex}

- **Syntax:** `ModPagespeedRedisDatabaseIndex number` (Apache) · `pagespeed RedisDatabaseIndex number;` (nginx) · `pagespeed RedisDatabaseIndex number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Redis server database index selection

#### RedisPassword {#redispassword}

- **Syntax:** `ModPagespeedRedisPassword value` (Apache) · `pagespeed RedisPassword value;` (nginx) · `pagespeed RedisPassword value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Password sent with AUTH after every Redis connect

#### RedisReconnectionDelayMs {#redisreconnectiondelayms}

- **Syntax:** `ModPagespeedRedisReconnectionDelayMs number` (Apache) · `pagespeed RedisReconnectionDelayMs number;` (nginx) · `pagespeed RedisReconnectionDelayMs number` (IIS)
- **Default:** `1000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Time to wait after unsuccessful reconnection before another attempt (ms)

#### RedisServer {#redisserver}

- **Syntax:** `ModPagespeedRedisServer value` (Apache) · `pagespeed RedisServer value;` (nginx) · `pagespeed RedisServer value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Redis server to use in format: &lt;host&gt;[:&lt;port&gt;] or unix:/&lt;socket path&gt;

#### RedisTimeoutUs {#redistimeoutus}

- **Syntax:** `ModPagespeedRedisTimeoutUs number` (Apache) · `pagespeed RedisTimeoutUs number;` (nginx) · `pagespeed RedisTimeoutUs number` (IIS)
- **Default:** `50000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Timeout for all Redis operations and connection (us)

#### RedisTTLSec {#redisttlsec}

- **Syntax:** `ModPagespeedRedisTTLSec number` (Apache) · `pagespeed RedisTTLSec number;` (nginx) · `pagespeed RedisTTLSec number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Redis key TTL to use (seconds)

#### RedisUser {#redisuser}

- **Syntax:** `ModPagespeedRedisUser value` (Apache) · `pagespeed RedisUser value;` (nginx) · `pagespeed RedisUser value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Redis ACL user for AUTH (used together with RedisPassword)

#### SharedMemoryLocks {#sharedmemorylocks}

- **Syntax:** `ModPagespeedSharedMemoryLocks on|off` (Apache) · `pagespeed SharedMemoryLocks on|off;` (nginx) · `pagespeed SharedMemoryLocks on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Use shared memory for internal named lock service

#### ShmMetadataCacheCheckpointIntervalSec {#shmmetadatacachecheckpointintervalsec}

- **Syntax:** `ModPagespeedShmMetadataCacheCheckpointIntervalSec number` (Apache) · `pagespeed ShmMetadataCacheCheckpointIntervalSec number;` (nginx) · `pagespeed ShmMetadataCacheCheckpointIntervalSec number` (IIS)
- **Default:** `300`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

How often to checkpoint the shared memory metadata cache to disk. Set to 0 to turn off checkpointing.

#### UseFallbackPropertyCacheValues {#usefallbackpropertycachevalues}

- **Syntax:** `ModPagespeedUseFallbackPropertyCacheValues on|off` (Apache) · `pagespeed UseFallbackPropertyCacheValues on|off;` (nginx) · `pagespeed UseFallbackPropertyCacheValues on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

If this is set to true, fallback values will be used from property cache if actual value is not present. Here fallback values means properties which are shared across all requests which have same url if query paramaters are removed. Example: http://www.test.com?a=1 and http://www.test.com?a=2 share same fallback properties though they are two different urls.

### Fetching and origins {#area-fetching-and-origins}

[`BlockingRewriteKey`](#blockingrewritekey), [`BlockingRewriteRefererUrls`](#blockingrewriterefererurls), [`CustomFetchHeader`](#customfetchheader), [`DangerPermitFetchFromUnknownHosts`](#dangerpermitfetchfromunknownhosts), [`ExperimentalMeasurementProxy`](#experimentalmeasurementproxy), [`FetcherTimeOutMs`](#fetchertimeoutms), [`FetchHttps`](#fetchhttps), [`FetchProxy`](#fetchproxy), [`FetchWithGzip`](#fetchwithgzip), [`NativeFetcherMaxKeepaliveRequests`](#nativefetchermaxkeepaliverequests), [`RateLimitBackgroundFetches`](#ratelimitbackgroundfetches), [`SlurpDirectory`](#slurpdirectory), [`SlurpFlushLimit`](#slurpflushlimit), [`SlurpReadOnly`](#slurpreadonly), [`SslCertDirectory`](#sslcertdirectory), [`SslCertFile`](#sslcertfile), [`TestProxy`](#testproxy), [`TestProxySlurp`](#testproxyslurp), [`UseNativeFetcher`](#usenativefetcher)

#### BlockingRewriteKey {#blockingrewritekey}

- **Syntax:** `ModPagespeedBlockingRewriteKey value` (Apache) · `pagespeed BlockingRewriteKey value;` (nginx) · `pagespeed BlockingRewriteKey value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

If the X-PSA-Pagespeed-Blocking-Rewrite header is present, and its value matches the configured value, ensure that all rewrites are completed before sending the response to the client.

#### BlockingRewriteRefererUrls {#blockingrewriterefererurls}

- **Syntax:** `ModPagespeedBlockingRewriteRefererUrls wildcard` (Apache) · `pagespeed BlockingRewriteRefererUrls wildcard;` (nginx) · `pagespeed BlockingRewriteRefererUrls wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

wildcard_spec for referer urls which trigger blocking rewrites

#### CustomFetchHeader {#customfetchheader}

- **Syntax:** `ModPagespeedCustomFetchHeader name value` (Apache) · `pagespeed CustomFetchHeader name value;` (nginx) · `pagespeed CustomFetchHeader name value` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Adds a request header to every fetch the module makes to an origin.

#### DangerPermitFetchFromUnknownHosts {#dangerpermitfetchfromunknownhosts}

- **Syntax:** `ModPagespeedDangerPermitFetchFromUnknownHosts on|off` (Apache) · `pagespeed DangerPermitFetchFromUnknownHosts on|off;` (nginx) · `pagespeed DangerPermitFetchFromUnknownHosts on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Disable security checks that prohibit fetching from hostnames mod_pagespeed does not know about

#### ExperimentalMeasurementProxy {#experimentalmeasurementproxy}

- **Syntax:** `ModPagespeedExperimentalMeasurementProxy https://root.domain password` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Experimental measurement proxy mode (Apache only).

#### FetcherTimeOutMs {#fetchertimeoutms}

- **Syntax:** `ModPagespeedFetcherTimeOutMs number` (Apache) · `pagespeed FetcherTimeOutMs number;` (nginx) · `pagespeed FetcherTimeOutMs number` (IIS)
- **Default:** `5000`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, match rules

Set internal fetcher timeout in milliseconds

#### FetchHttps {#fetchhttps}

- **Syntax:** `ModPagespeedFetchHttps value` (Apache) · `pagespeed FetchHttps value;` (nginx) · `pagespeed FetchHttps value` (IIS)
- **Default:** `enable`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Controls direct fetching of HTTPS resources. Value is comma-separated list of keywords: enable,disable,allow_self_signed,allow_unknown_certificate_authority,allow_certificate_not_yet_valid

#### FetchProxy {#fetchproxy}

- **Syntax:** `ModPagespeedFetchProxy value` (Apache) · `pagespeed FetchProxy value;` (nginx) · `pagespeed FetchProxy value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the fetch proxy

#### FetchWithGzip {#fetchwithgzip}

- **Syntax:** `ModPagespeedFetchWithGzip on|off` (Apache) · `pagespeed FetchWithGzip on|off;` (nginx) · `pagespeed FetchWithGzip on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host (tolerated) · nginx: http · IIS: pagespeed.config, outside match rules

Request http content from origin servers using gzip

#### NativeFetcherMaxKeepaliveRequests {#nativefetchermaxkeepaliverequests}

- **Syntax:** `pagespeed NativeFetcherMaxKeepaliveRequests number;` (nginx)
- **Default:** —
- **Context:** nginx: http
- **Platform:** nginx only

Maximum number of requests the native fetcher sends over one keep-alive connection before it opens a new one (a positive integer).

#### RateLimitBackgroundFetches {#ratelimitbackgroundfetches}

- **Syntax:** `ModPagespeedRateLimitBackgroundFetches on|off` (Apache) · `pagespeed RateLimitBackgroundFetches on|off;` (nginx) · `pagespeed RateLimitBackgroundFetches on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Rate-limit the number of background HTTP fetches done at once

#### SlurpDirectory {#slurpdirectory}

- **Syntax:** `ModPagespeedSlurpDirectory value` (Apache) · `pagespeed SlurpDirectory value;` (nginx) · `pagespeed SlurpDirectory value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Directory from which to read slurped resources

#### SlurpFlushLimit {#slurpflushlimit}

- **Syntax:** `ModPagespeedSlurpFlushLimit number` (Apache) · `pagespeed SlurpFlushLimit number;` (nginx) · `pagespeed SlurpFlushLimit number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Set the maximum byte size for the slurped content to hold before a flush

#### SlurpReadOnly {#slurpreadonly}

- **Syntax:** `ModPagespeedSlurpReadOnly on|off` (Apache) · `pagespeed SlurpReadOnly on|off;` (nginx) · `pagespeed SlurpReadOnly on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Only read from the slurped directory, fail to fetch URLs not already in the slurped directory

#### SslCertDirectory {#sslcertdirectory}

- **Syntax:** `ModPagespeedSslCertDirectory value` (Apache) · `pagespeed SslCertDirectory value;` (nginx) · `pagespeed SslCertDirectory value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Directory to find SSL certificates.

#### SslCertFile {#sslcertfile}

- **Syntax:** `ModPagespeedSslCertFile value` (Apache) · `pagespeed SslCertFile value;` (nginx) · `pagespeed SslCertFile value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

File with SSL certificates.

#### TestProxy {#testproxy}

- **Syntax:** `ModPagespeedTestProxy on|off` (Apache) · `pagespeed TestProxy on|off;` (nginx) · `pagespeed TestProxy on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Direct non-PageSpeed URLs to a fetcher, acting as a simple proxy. Meant for test use only

#### TestProxySlurp {#testproxyslurp}

- **Syntax:** `ModPagespeedTestProxySlurp value` (Apache) · `pagespeed TestProxySlurp value;` (nginx) · `pagespeed TestProxySlurp value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

If set, the fetcher used by the TestProxy mode will be a readonly slurp fetcher from the given directory

#### UseNativeFetcher {#usenativefetcher}

- **Syntax:** `pagespeed UseNativeFetcher on|off;` (nginx)
- **Default:** —
- **Context:** nginx: http
- **Platform:** nginx only

Use nginx's own event-driven fetcher for the resource fetches the module makes, instead of the built-in serf fetcher.

Settable in the `http` block only. The native fetcher handles HTTP and HTTPS
fetches.

### HTML rewriting and page hints {#area-html-rewriting-and-page-hints}

[`AmpLinkPattern`](#amplinkpattern), [`BeaconReinstrumentTimeSec`](#beaconreinstrumenttimesec), [`BeaconUrl`](#beaconurl), [`EnableExtendedInstrumentation`](#enableextendedinstrumentation), [`FlushBufferLimitBytes`](#flushbufferlimitbytes), [`FlushHtml`](#flushhtml), [`FollowFlushes`](#followflushes), [`IdleFlushTimeMs`](#idleflushtimems), [`LowercaseHtmlNames`](#lowercasehtmlnames), [`MaxHtmlParseBytes`](#maxhtmlparsebytes), [`RetainComment`](#retaincomment)

#### AmpLinkPattern {#amplinkpattern}

- **Syntax:** `pagespeed AmpLinkPattern value;` (nginx) · `pagespeed AmpLinkPattern value` (IIS)
- **Default:** `(empty)`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### BeaconReinstrumentTimeSec {#beaconreinstrumenttimesec}

- **Syntax:** `ModPagespeedBeaconReinstrumentTimeSec number` (Apache) · `pagespeed BeaconReinstrumentTimeSec number;` (nginx) · `pagespeed BeaconReinstrumentTimeSec number` (IIS)
- **Default:** `5`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

How often (in seconds) to reinstrument pages with beacons. This is used for both critical image beaconing, and for the prioritize_critical_css filter.

#### BeaconUrl {#beaconurl}

- **Syntax:** `ModPagespeedBeaconUrl value` (Apache) · `pagespeed BeaconUrl value;` (nginx) · `pagespeed BeaconUrl value` (IIS)
- **Default:** `/mod_pagespeed_beacon`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

URL for beacon callback injected by add_instrumentation.

#### EnableExtendedInstrumentation {#enableextendedinstrumentation}

- **Syntax:** `ModPagespeedEnableExtendedInstrumentation on|off` (Apache) · `pagespeed EnableExtendedInstrumentation on|off;` (nginx) · `pagespeed EnableExtendedInstrumentation on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

If set to true, addition instrumentation js is added to that page that the beacon can collect more information.

#### FlushBufferLimitBytes {#flushbufferlimitbytes}

- **Syntax:** `pagespeed FlushBufferLimitBytes number;` (nginx) · `pagespeed FlushBufferLimitBytes number` (IIS)
- **Default:** `102400`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### FlushHtml {#flushhtml}

- **Syntax:** `ModPagespeedFlushHtml on|off` (Apache) · `pagespeed FlushHtml on|off;` (nginx) · `pagespeed FlushHtml on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host · nginx: http, server · IIS: pagespeed.config, outside match rules

Enable auto-flush heuristics for HTML in full proxy mode

#### FollowFlushes {#followflushes}

- **Syntax:** `ModPagespeedFollowFlushes on|off` (Apache) · `pagespeed FollowFlushes on|off;` (nginx) · `pagespeed FollowFlushes on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Attempt to mirror incoming flushes for html streams in the output when ProxyFetch is used.

#### IdleFlushTimeMs {#idleflushtimems}

- **Syntax:** `pagespeed IdleFlushTimeMs number;` (nginx) · `pagespeed IdleFlushTimeMs number` (IIS)
- **Default:** `10`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### LowercaseHtmlNames {#lowercasehtmlnames}

- **Syntax:** `ModPagespeedLowercaseHtmlNames on|off` (Apache) · `pagespeed LowercaseHtmlNames on|off;` (nginx) · `pagespeed LowercaseHtmlNames on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Lowercase tag and attribute names for HTML.

When enabled, HTML tag and attribute names are lowercased during parsing.

#### MaxHtmlParseBytes {#maxhtmlparsebytes}

- **Syntax:** `ModPagespeedMaxHtmlParseBytes number` (Apache) · `pagespeed MaxHtmlParseBytes number;` (nginx) · `pagespeed MaxHtmlParseBytes number` (IIS)
- **Default:** `-1`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Maximum number of bytes of HTML that we parse, before redirecting to ?ModPagespeed=off

#### RetainComment {#retaincomment}

- **Syntax:** `ModPagespeedRetainComment wildcard` (Apache) · `pagespeed RetainComment wildcard;` (nginx) · `pagespeed RetainComment wildcard` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Retain HTML comments matching wildcard, even with remove_comments enabled

### Threads and limits {#area-threads-and-limits}

[`DebugOutputMaxBytes`](#debugoutputmaxbytes), [`NumExpensiveRewriteThreads`](#numexpensiverewritethreads), [`NumRewriteThreads`](#numrewritethreads), [`PublicAdminAccess`](#publicadminaccess)

#### DebugOutputMaxBytes {#debugoutputmaxbytes}

- **Syntax:** `ModPagespeedDebugOutputMaxBytes number` (Apache) · `pagespeed DebugOutputMaxBytes number;` (nginx) · `pagespeed DebugOutputMaxBytes number` (IIS)
- **Default:** `1048576`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Most bytes of debug comments written into one document (0 = unbounded).

#### NumExpensiveRewriteThreads {#numexpensiverewritethreads}

- **Syntax:** `ModPagespeedNumExpensiveRewriteThreads auto|number` (Apache) · `pagespeed NumExpensiveRewriteThreads auto|number;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Threads per process for heavy optimization work such as image transcoding. auto (or 0) sizes the pool from the CPUs the process may use; a positive integer sets it exactly.

Global, like `NumRewriteThreads`. See [Optimization threads](#optimization-threads).

#### NumRewriteThreads {#numrewritethreads}

- **Syntax:** `ModPagespeedNumRewriteThreads auto|number` (Apache) · `pagespeed NumRewriteThreads auto|number;` (nginx)
- **Default:** —
- **Context:** Apache: server config · nginx: http

Threads per process for short, latency-sensitive optimization work. auto (or 0) sizes the pool from the CPUs the process may use; a positive integer sets it exactly.

Global: set it once per server, not per virtual host or location. How `auto`
resolves per server, and when to override it, is in
[Optimization threads](#optimization-threads).

#### PublicAdminAccess {#publicadminaccess}

- **Syntax:** `ModPagespeedPublicAdminAccess on|off` (Apache) · `pagespeed PublicAdminAccess on|off;` (nginx) · `pagespeed PublicAdminAccess on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

OPT-IN, default off. Declares that the admin pages are reachable from the network on purpose. When on, the loopback warning for admin handlers is replaced by one informational line, and the console shows an informational finding instead of the exposure warning. Access control is unchanged.

### Experiments and analytics {#area-experiments-and-analytics}

[`EnrollExperiment`](#enrollexperiment), [`ExperimentCookieDurationMs`](#experimentcookiedurationms), [`ExperimentSlot`](#experimentslot), [`ExperimentSpec`](#experimentspec), [`Noop`](#noop), [`RunExperiment`](#runexperiment)

#### EnrollExperiment {#enrollexperiment}

- **Syntax:** `ModPagespeedEnrollExperiment number` (Apache) · `pagespeed EnrollExperiment number;` (nginx) · `pagespeed EnrollExperiment number` (IIS)
- **Default:** `-2`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Assign users to a specific experiment setting.

#### ExperimentCookieDurationMs {#experimentcookiedurationms}

- **Syntax:** `pagespeed ExperimentCookieDurationMs number;` (nginx) · `pagespeed ExperimentCookieDurationMs number` (IIS)
- **Default:** `604800000`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ExperimentSlot {#experimentslot}

- **Syntax:** `pagespeed ExperimentSlot number;` (nginx) · `pagespeed ExperimentSlot number` (IIS)
- **Default:** `1`
- **Context:** nginx: http, server, location · IIS: pagespeed.config, match rules

_The module source carries no help text for this option._

#### ExperimentSpec {#experimentspec}

- **Syntax:** `ModPagespeedExperimentSpec spec` (Apache) · `pagespeed ExperimentSpec spec;` (nginx) · `pagespeed ExperimentSpec spec` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Configuration for one side of an experiment in the form: 'id= ;enabled= ;disabled= ;ga= ;percent= ...' The 'ga=' component is accepted for compatibility and ignored.

#### Noop {#noop}

- **Syntax:** `ModPagespeedNoop number` (Apache) · `pagespeed Noop number;` (nginx) · `pagespeed Noop number` (IIS)
- **Default:** `0`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Meaningless integer option for browser cache-busting in query-params

#### RunExperiment {#runexperiment}

- **Syntax:** `ModPagespeedRunExperiment on|off` (Apache) · `pagespeed RunExperiment on|off;` (nginx) · `pagespeed RunExperiment on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Run an experiment to test the effectiveness of rewriters.

### Other {#area-other}

[`DetectHydratedPages`](#detecthydratedpages), [`ExposeVersionInHeaders`](#exposeversioninheaders)

#### DetectHydratedPages {#detecthydratedpages}

- **Syntax:** `ModPagespeedDetectHydratedPages on|off` (Apache) · `pagespeed DetectHydratedPages on|off;` (nginx) · `pagespeed DetectHydratedPages on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Keep stylesheet and script URLs and inline styles unchanged on pages a client-side framework will hydrate. Server-wide only.

#### ExposeVersionInHeaders {#exposeversioninheaders}

- **Syntax:** `ModPagespeedExposeVersionInHeaders on|off` (Apache) · `pagespeed ExposeVersionInHeaders on|off;` (nginx) · `pagespeed ExposeVersionInHeaders on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Report the product version in the X-Mod-Pagespeed / X-Page-Speed response header instead of the generic value "on". Server-wide only; an explicit XHeaderValue still wins.

### Deprecated and ignored {#area-deprecated-and-ignored}

[`AllowVaryOn`](#allowvaryon), [`AnalyticsID`](#analyticsid), [`CollectRefererStatistics`](#collectrefererstatistics), [`ContentExperimentID`](#contentexperimentid), [`ContentExperimentVariantID`](#contentexperimentvariantid), [`DisableForBots`](#disableforbots), [`DistributedRewriteKey`](#distributedrewritekey), [`DistributedRewriteServers`](#distributedrewriteservers), [`DistributedRewriteTimeoutMs`](#distributedrewritetimeoutms), [`DistributeFetches`](#distributefetches), [`ExperimentalCentralControllerPort`](#experimentalcentralcontrollerport), [`ExperimentalPopularityContestMaxInFlight`](#experimentalpopularitycontestmaxinflight), [`ExperimentalPopularityContestMaxQueueSize`](#experimentalpopularitycontestmaxqueuesize), [`ExperimentVariable`](#experimentvariable), [`FetchFromModSpdy`](#fetchfrommodspdy), [`FileCacheCleanIntervalMs`](#filecachecleanintervalms), [`FileCacheInodeLimit`](#filecacheinodelimit), [`GeneratedFilePrefix`](#generatedfileprefix), [`HashRefererStatistics`](#hashrefererstatistics), [`If`](#if), [`ImageWebpRecompressionQuality`](#imagewebprecompressionquality), [`ImageWebpRecompressionQualityForSmallScreens`](#imagewebprecompressionqualityforsmallscreens), [`ImgInlineMaxBytes`](#imginlinemaxbytes), [`ImgMaxRewritesAtOnce`](#imgmaxrewritesatonce), [`IncreaseSpeedTracking`](#increasespeedtracking), [`InheritVHostConfig`](#inheritvhostconfig), [`LogMobilizationSamples`](#logmobilizationsamples), [`MaxPrefetchJsElements`](#maxprefetchjselements), [`NumShards`](#numshards), [`PrivateNotVaryForIE`](#privatenotvaryforie), [`RefererStatisticsOutputLevel`](#refererstatisticsoutputlevel), [`StatisticsLoggingFile`](#statisticsloggingfile), [`UrlPrefix`](#urlprefix), [`UseAnalyticsJs`](#useanalyticsjs), [`UseExperimentalJsMinifier`](#useexperimentaljsminifier)

#### AllowVaryOn {#allowvaryon}

- **Syntax:** `ModPagespeedAllowVaryOn value` (Apache) · `pagespeed AllowVaryOn value;` (nginx) · `pagespeed AllowVaryOn value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored: it no longer affects in-place optimization. With the optimizer worker attached, in-place images are served in the format the request's Accept header selects, with Vary: Accept.

#### AnalyticsID {#analyticsid}

- **Syntax:** `ModPagespeedAnalyticsID value` (Apache) · `pagespeed AnalyticsID value;` (nginx) · `pagespeed AnalyticsID value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored (configured the insert_ga filter).

#### CollectRefererStatistics {#collectrefererstatistics}

- **Syntax:** `ModPagespeedCollectRefererStatistics value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### ContentExperimentID {#contentexperimentid}

- **Syntax:** `ModPagespeedContentExperimentID value` (Apache) · `pagespeed ContentExperimentID value;` (nginx) · `pagespeed ContentExperimentID value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored (configured the insert_ga filter).

#### ContentExperimentVariantID {#contentexperimentvariantid}

- **Syntax:** `ModPagespeedContentExperimentVariantID value` (Apache) · `pagespeed ContentExperimentVariantID value;` (nginx) · `pagespeed ContentExperimentVariantID value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored (configured the insert_ga filter).

#### DisableForBots {#disableforbots}

- **Syntax:** `ModPagespeedDisableForBots value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### DistributedRewriteKey {#distributedrewritekey}

- **Syntax:** `ModPagespeedDistributedRewriteKey value` (Apache) · `pagespeed DistributedRewriteKey value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### DistributedRewriteServers {#distributedrewriteservers}

- **Syntax:** `ModPagespeedDistributedRewriteServers value` (Apache) · `pagespeed DistributedRewriteServers value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### DistributedRewriteTimeoutMs {#distributedrewritetimeoutms}

- **Syntax:** `ModPagespeedDistributedRewriteTimeoutMs value` (Apache) · `pagespeed DistributedRewriteTimeoutMs value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### DistributeFetches {#distributefetches}

- **Syntax:** `ModPagespeedDistributeFetches value` (Apache) · `pagespeed DistributeFetches value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### ExperimentalCentralControllerPort {#experimentalcentralcontrollerport}

- **Syntax:** `ModPagespeedExperimentalCentralControllerPort value` (Apache) · `pagespeed ExperimentalCentralControllerPort value;` (nginx) · `pagespeed ExperimentalCentralControllerPort value` (IIS)
- **Default:** `(empty)`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Deprecated and ignored: the experimental gRPC central controller was removed

#### ExperimentalPopularityContestMaxInFlight {#experimentalpopularitycontestmaxinflight}

- **Syntax:** `ModPagespeedExperimentalPopularityContestMaxInFlight number` (Apache) · `pagespeed ExperimentalPopularityContestMaxInFlight number;` (nginx) · `pagespeed ExperimentalPopularityContestMaxInFlight number` (IIS)
- **Default:** `10`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Deprecated and ignored: the experimental gRPC central controller was removed

#### ExperimentalPopularityContestMaxQueueSize {#experimentalpopularitycontestmaxqueuesize}

- **Syntax:** `ModPagespeedExperimentalPopularityContestMaxQueueSize number` (Apache) · `pagespeed ExperimentalPopularityContestMaxQueueSize number;` (nginx) · `pagespeed ExperimentalPopularityContestMaxQueueSize number` (IIS)
- **Default:** `1000`
- **Context:** Apache: server config · nginx: http · IIS: pagespeed.config, outside match rules

Deprecated and ignored: the experimental gRPC central controller was removed

#### ExperimentVariable {#experimentvariable}

- **Syntax:** `ModPagespeedExperimentVariable slot` (Apache) · `pagespeed ExperimentVariable slot;` (nginx) · `pagespeed ExperimentVariable slot` (IIS)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored (configured the insert_ga filter).

#### FetchFromModSpdy {#fetchfrommodspdy}

- **Syntax:** `ModPagespeedFetchFromModSpdy value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### FileCacheCleanIntervalMs {#filecachecleanintervalms}

- **Syntax:** `ModPagespeedFileCacheCleanIntervalMs value` (Apache) · `pagespeed FileCacheCleanIntervalMs value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### FileCacheInodeLimit {#filecacheinodelimit}

- **Syntax:** `ModPagespeedFileCacheInodeLimit value` (Apache) · `pagespeed FileCacheInodeLimit value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host · nginx: http, server

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### GeneratedFilePrefix {#generatedfileprefix}

- **Syntax:** `ModPagespeedGeneratedFilePrefix value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### HashRefererStatistics {#hashrefererstatistics}

- **Syntax:** `ModPagespeedHashRefererStatistics value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### If {#if}

- **Syntax:** `<ModPagespeedIf spdy|!spdy>` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Conditionally apply some mod_pagespeed options. Possible arguments: spdy, !spdy

#### ImageWebpRecompressionQuality {#imagewebprecompressionquality}

- **Syntax:** `ModPagespeedImageWebpRecompressionQuality number` (Apache) · `pagespeed ImageWebpRecompressionQuality number;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · also per request (query parameter)

Former name of WebpRecompressionQuality; accepted and mapped to it.

#### ImageWebpRecompressionQualityForSmallScreens {#imagewebprecompressionqualityforsmallscreens}

- **Syntax:** `ModPagespeedImageWebpRecompressionQualityForSmallScreens number` (Apache) · `pagespeed ImageWebpRecompressionQualityForSmallScreens number;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · also per request (query parameter)

Former name of WebpRecompressionQualityForSmallScreens; accepted and mapped to it.

#### ImgInlineMaxBytes {#imginlinemaxbytes}

- **Syntax:** `ModPagespeedImgInlineMaxBytes number` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · also per request (query parameter)
- **Platform:** Apache only

Former name of ImageInlineMaxBytes; accepted and mapped to it.

#### ImgMaxRewritesAtOnce {#imgmaxrewritesatonce}

- **Syntax:** `ModPagespeedImgMaxRewritesAtOnce number` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host (tolerated)
- **Platform:** Apache only

Former name of ImageMaxRewritesAtOnce; accepted and mapped to it.

#### IncreaseSpeedTracking {#increasespeedtracking}

- **Syntax:** `ModPagespeedIncreaseSpeedTracking on|off` (Apache) · `pagespeed IncreaseSpeedTracking on|off;` (nginx) · `pagespeed IncreaseSpeedTracking on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored (configured the insert_ga filter).

#### InheritVHostConfig {#inheritvhostconfig}

- **Syntax:** `ModPagespeedInheritVHostConfig value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### LogMobilizationSamples {#logmobilizationsamples}

- **Syntax:** `ModPagespeedLogMobilizationSamples on|off` (Apache) · `pagespeed LogMobilizationSamples on|off;` (nginx) · `pagespeed LogMobilizationSamples on|off` (IIS)
- **Default:** `off`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored (configured the mobilize filter).

#### MaxPrefetchJsElements {#maxprefetchjselements}

- **Syntax:** `ModPagespeedMaxPrefetchJsElements value` (Apache) · `pagespeed MaxPrefetchJsElements value;` (nginx)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location

Removed option. Apache still accepts the name and ignores it, with a warning. nginx does not recognize it: `nginx -t` fails and nginx will not start until you remove the line. It has no replacement.

#### NumShards {#numshards}

- **Syntax:** `ModPagespeedNumShards value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### PrivateNotVaryForIE {#privatenotvaryforie}

- **Syntax:** `ModPagespeedPrivateNotVaryForIE on|off` (Apache) · `pagespeed PrivateNotVaryForIE on|off;` (nginx) · `pagespeed PrivateNotVaryForIE on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored: in-place optimized resources are no longer downgraded to Cache-Control: private for Internet Explorer. With the optimizer worker attached, in-place images vary by the request's Accept header and carry Vary: Accept.

#### RefererStatisticsOutputLevel {#refererstatisticsoutputlevel}

- **Syntax:** `ModPagespeedRefererStatisticsOutputLevel value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### StatisticsLoggingFile {#statisticsloggingfile}

- **Syntax:** `ModPagespeedStatisticsLoggingFile value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host, directory, .htaccess
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### UrlPrefix {#urlprefix}

- **Syntax:** `ModPagespeedUrlPrefix value` (Apache)
- **Default:** —
- **Context:** Apache: server config, virtual host
- **Platform:** Apache only

Removed option; the name is still accepted and ignored.

#### UseAnalyticsJs {#useanalyticsjs}

- **Syntax:** `ModPagespeedUseAnalyticsJs on|off` (Apache) · `pagespeed UseAnalyticsJs on|off;` (nginx) · `pagespeed UseAnalyticsJs on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules · also per request (query parameter)

Deprecated and ignored (configured the insert_ga filter).

#### UseExperimentalJsMinifier {#useexperimentaljsminifier}

- **Syntax:** `ModPagespeedUseExperimentalJsMinifier on|off` (Apache) · `pagespeed UseExperimentalJsMinifier on|off;` (nginx) · `pagespeed UseExperimentalJsMinifier on|off` (IIS)
- **Default:** `on`
- **Context:** Apache: server config, virtual host, directory, .htaccess · nginx: http, server, location · IIS: pagespeed.config, match rules

Deprecated and ignored: the legacy JavaScript minifier was removed; the tokenizer-based minifier is the only JavaScript minifier

Deprecated since v1.15.0+r21: the tokenizer-based minifier is the only
JavaScript minifier. The directive is accepted for compatibility, ignored, and
logs a deprecation warning at configuration load. Remove it from your
configuration.

<!-- generated:end module-directives -->

## Optimization threads

Optimization work runs on two thread pools, separate from the threads that serve
requests. The _rewrite_ pool handles short, latency-sensitive bookkeeping; the
_expensive rewrite_ pool handles heavy CPU work such as image transcoding, so a
large image cannot hold up everything else. This is the in-process module's own
thread configuration; the optimizer worker's `--num-threads` and
`--ram-cache-size` flags are a different setting, on the
[worker page](/docs/worker-configuration/#threading).

In v1.15.0+r21 and later both pools size themselves.
[`NumRewriteThreads`](#numrewritethreads) and
[`NumExpensiveRewriteThreads`](#numexpensiverewritethreads) default to `auto`,
and mod_pagespeed resolves them at startup by:

1. Taking the CPUs the process is **actually permitted to use** — not the
   host's core count. A CPU quota (a container limit or a systemd unit) and a
   CPU affinity mask both count. A 2-CPU container on a 64-core host sees 2.
2. Allowing optimization at most **half** of those.
3. Dividing by the number of **peer processes** the server is configured to run,
   because each process builds its own pools. Without this step a server with
   many children would multiply its thread count by the number of children.
4. Giving the result to **each** pool, never fewer than one thread.

The resolved counts, and the numbers they were derived from, are written to the
error log at startup.

### What this means per server

<div data-platform="apache" data-platform-label="Apache">

Apache reports its configured child-process ceiling, so the divisor is real:
`MaxRequestWorkers / ThreadsPerChild` on `worker` and `event`, and
`MaxRequestWorkers` on `prefork` — in both cases capped by `ServerLimit`. If you
have lowered `ServerLimit`, that is the number in effect, not the division.

A stock `event` configuration allows 400 workers at 25 threads per child, so the
divisor is 16 and a server resolves to **one thread per pool** unless it has a
great many cores. That is the intended answer: the ceiling counts children the
server is _allowed_ to start, and sizing each child as though it were alone on
the machine is what oversubscribes it. A server deliberately configured with few
children on a many-core machine gets proportionally more.

`prefork` is not threaded and resolves to one thread per pool, as it always has.

</div>

<div data-platform="nginx" data-platform-label="nginx">

nginx divides by `worker_processes`. With `worker_processes auto` (one worker
per core) the result is **one thread per pool**. Workers pinned with
`worker_cpu_affinity` also get one thread per pool, because the CPU budget is
read before the workers are pinned and dividing it would size each worker for the
whole machine. A server with few workers on a many-core machine gets
proportionally more.

This is unchanged from earlier releases. If you have set `NumRewriteThreads` or
`NumExpensiveRewriteThreads` explicitly, your values continue to apply exactly
as before.

</div>

<div data-platform="iis" data-platform-label="IIS">

The IIS module sizes its own pools and does not yet consult these directives.
Its behaviour is unchanged in this release. See
[IIS tuning](/docs/iis-configuration/#iis-tuning).

</div>

### Setting the counts explicitly

An explicit positive value overrides the computed one entirely.

| Value            | Effect                                                               |
| ---------------- | -------------------------------------------------------------------- |
| `auto`           | Resolve as described above. This is the default.                     |
| `0`              | An alias for `auto`.                                                 |
| positive integer | Use exactly this many threads per process, ignoring the computation. |
| negative         | Rejected when the configuration is read.                             |

A negative value is rejected at configuration-read time rather than being
clamped, which on Apache means the server does not start. Very large explicit
values are capped, with a warning naming both the value you asked for and the
value in effect.

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedNumRewriteThreads 4
ModPagespeedNumExpensiveRewriteThreads 4
```

</div>

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed NumRewriteThreads 4;
pagespeed NumExpensiveRewriteThreads 4;
```

</div>

Both directives are **global**. They cannot be set per virtual host or per
location: the pools are built once per process, before any virtual host is
consulted.

### When to change them

Most servers should leave these alone. Reach for them when the log line at
startup shows a resolved count that does not match your deployment — most often
a single-process server on a many-core machine, or a server whose configured
child ceiling is far above the number of children it will really run.

If you raise them, watch request latency rather than only optimization
throughput. The reason optimization is capped at half the machine is that the
remaining half is serving traffic.

One case where raising `NumExpensiveRewriteThreads` helps is a first view of an
image-heavy page that stalls on its stylesheets and scripts while images are
being optimized. The cost is more concurrent image work, so more CPU and memory at
peak. See
[Stylesheets and scripts respond slowly while images are being optimized](/docs/troubleshooting/#slow-ce-while-optimizing-images).

## URL segment length limits {#max-url-segments}

[`MaxSegmentLength`](#maxsegmentlength) sets the maximum length (in characters)
of any single URL segment — the text between two `/` separators — that the
module will produce when it combines or rewrites resources.

Apache servers historically capped URL segments at about 250 characters per
segment. mod_pagespeed circumvents that limit when running under Apache, but
intermediate proxies or CDNs in front of your origin may re-impose it. If a
downstream component rejects long `.pagespeed.` URLs, lower this value so
mod_pagespeed produces shorter combined URLs.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed MaxSegmentLength 250;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedMaxSegmentLength 250
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed MaxSegmentLength 250
```

</div>

This directive applies only to the URLs mod_pagespeed generates (for example,
when combining CSS or JavaScript). It does not limit which inbound request
URLs the module will rewrite.

## Version in response headers {#expose-version-in-headers}

From 2.7.0, the module's response header (`X-Mod-Pagespeed` on Apache,
`X-Page-Speed` on nginx and IIS) carries the generic value `on` by
default. It confirms that the module handled the response and does not disclose
which version is installed. Detection by header presence is unchanged. Releases
before 2.7.0 send the version in that header.

To send the module version instead, set `ExposeVersionInHeaders` once for the
whole server. It is a server-wide boolean (default off) and is rejected inside a
virtual host or location. Releases before 2.7.0 do not know the directive and
refuse to load a configuration that contains it (`apache2ctl configtest` and
`nginx -t` fail), so add it in the same step as the upgrade to 2.7.0, never
before.

```apache
# Apache, global server configuration
ModPagespeedExposeVersionInHeaders on
```

```nginx
# nginx, http block
pagespeed ExposeVersionInHeaders on;
```

```text
# IIS: the server-wide %ProgramData%\We-Amp\PageSpeed\pagespeed.config,
# not a site's own pagespeed.config
pagespeed ExposeVersionInHeaders on
```

Precedence for the header value: a custom [`XHeaderValue`](#xheadervalue) wins,
then `ExposeVersionInHeaders`, then `on`. The Docker reverse proxy and the ASP.NET
Core middleware use `X-PageSpeed` for the cache outcome (`HIT`, `MISS`) and never
send a version.

## Behind a reverse proxy

When mod_pagespeed runs behind a reverse proxy (such as nginx, Varnish, or a
CDN), keep the following in mind:

- The proxy must forward the original `Host` header to the backend so that
  mod_pagespeed generates correct URLs for optimized resources.
- If the proxy terminates TLS, configure mod_pagespeed to recognize the
  `X-Forwarded-Proto` header ([`RespectXForwardedProto`](#respectxforwardedproto))
  so that it generates `https://` URLs for optimized resources. See
  [HTTPS configuration](/docs/https-configuration/) for the details.
- Ensure that `.pagespeed.` resource URLs are routed to the backend running
  mod_pagespeed. The proxy must not cache these resources independently unless
  you configure cache lifetimes carefully.
- If the proxy strips or modifies response headers, verify that
  mod_pagespeed's `X-Mod-Pagespeed` or `X-Page-Speed` header and
  `Cache-Control` directives pass through intact.

See also [CDN integration](/docs/cdn-integration/).

## See also

- [Filter selection](/docs/filter-selection/) — rewrite levels and enabling or disabling filters
- [PageSpeed filters](/docs/filters/) — every filter, what it does and which level enables it
- [Cache modes](/docs/cache-modes/) and [Domain configuration](/docs/domain-configuration/)
- [Worker and reverse-proxy configuration](/docs/worker-configuration/) — the Docker shape and the optimizer worker
- [PageSpeed markers reference](/pagespeed-markers/) — the attributes and headers these directives produce at runtime

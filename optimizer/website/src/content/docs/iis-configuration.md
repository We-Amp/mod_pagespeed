---
title: 'IIS configuration'
description: 'mod_pagespeed on IIS via pagespeed.config, the native IIS module and successor to IISpeed: directive syntax, path matching, server and site-level config.'
order: 27
group: 'Configure'
lastUpdated: 2026-10-22
---

mod_pagespeed runs on IIS as a native, in-process module — the successor to IISpeed. This page documents the `pagespeed.config` file format it reads. For the directives that apply across every platform (Apache, nginx, IIS), see [Configuration](/docs/configuration/); to install the module first, see [Install on IIS](/docs/install-iis/).

## Install the module {#install-the-module}

<a id="requirements"></a><a id="migrating-from-iispeed"></a><a id="iis-express"></a><a id="verify-it-works"></a>Installation moved to [Install on IIS](/docs/install-iis/): requirements, the signed MSI, IIS Express and the optional optimizer service. Coming from IISpeed, read [Migrate from IISpeed](/docs/migrate-from-iispeed/) first.

### Disable optimization

To disable optimization for a site, either delete or rename its `pagespeed.config` file, or add:

```text
pagespeed off
```

## File format

`pagespeed.config` is a flat text file with one directive per line. Lines starting with `#` are comments.

Since v1.15.0+r18, config parsing is more robust and more diagnosable: a malformed line no longer prevents module startup, unknown options are reported instead of silently ignored, and option scoping is enforced.

```text
# Enable optimization
pagespeed on

# Set the cache path
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache

# Enable specific filters
pagespeed EnableFilters collapse_whitespace,remove_comments
pagespeed DisableFilters combine_css
```

### Directive prefixes

Three directive prefixes are accepted:

| Prefix         | Example                         |
| -------------- | ------------------------------- |
| `pagespeed`    | `pagespeed EnableFilters ...`   |
| `ModPagespeed` | `ModPagespeedEnableFilters ...` |
| `iispeed`      | `iispeed EnableFilters ...`     |

The `pagespeed` prefix is recommended. `ModPagespeed` and `iispeed` are accepted for compatibility with Apache configurations and legacy IISpeed installations.

## Config file locations

The module searches for configuration files in two locations:

### Server-level config

```text
%ProgramData%\We-Amp\PageSpeed\pagespeed.config
```

The server-level file is `%ProgramData%\We-Amp\PageSpeed\pagespeed.config`. Installs upgraded from IISpeed also read `%ProgramData%\We-Amp\IISWebSpeed\pagespeed.config`. Settings here are the defaults for every website that has its own `pagespeed.config`; this file alone turns no website on.

### Site-level config

```text
<website root>\pagespeed.config
```

Place a `pagespeed.config` file in the website's physical root directory (e.g., `C:\inetpub\wwwroot\pagespeed.config`). Site-level settings override server-level settings. The `FileCachePath` must be included in per-site configs if not set in the server-level config.

### Lookup order

1. Check for `pagespeed.config` in the website root
2. If not found, check for `iiswebspeed.config` in the website root (the filename used by IISpeed installs)
3. Load the server-level `pagespeed.config` (or `iiswebspeed.config`) as the base configuration
4. Merge site-level settings on top of server-level settings

### Migration note

If you are migrating from IISpeed, the module accepts both `pagespeed.config` (preferred) and `iiswebspeed.config` (the filename used by IISpeed installs). When both files exist in the same directory, `pagespeed.config` takes priority. Rename your `iiswebspeed.config` to `pagespeed.config` when convenient — no content changes are needed.

## Cache directory

The IIS worker process needs write access to the cache directory set by `FileCachePath`. The installer creates the default cache directory and grants the worker identity the access it needs, so a standard install requires no manual setup.

Set the path explicitly with `FileCachePath`:

```text
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
```

To validate the cache path, make a request to any page on your server and check that files appear in the cache directory.

### Automatic cache-directory creation

From **v1.1.0+r11** onward, the module creates each website's cache subdirectory on first request and grants the worker identity write access — no manual `mkdir` or permission step. This works whether `FileCachePath` is under the `%ProgramData%\We-Amp\PageSpeed\` tree or the `%ProgramData%\We-Amp\IISWebSpeed\` tree used by installs upgraded from IISpeed, so a cache path inherited from an IISpeed install keeps working after an upgrade with no configuration change.

To require that the cache directory already exist instead, turn auto-creation off:

```text
pagespeed AutoCreateCachePath off
```

### "FileCachePath does not exist"

A site that serves a local-only diagnostic page reporting that the configured `FileCachePath` does not exist is running a build from before automatic cache-directory creation (earlier than v1.1.0+r11). Upgrade to the current release and the directory is created for you. To fix it in place on an older build, create the directory and grant the worker identity — the app pool identity, or `IIS_IUSRS` — Modify access, then recycle the app pool.

## Environment variable expansion

Windows environment variables are expanded in path values:

```text
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
pagespeed LogDir %ProgramData%\We-Amp\PageSpeed\Logs
```

Common variables:

| Variable        | Typical value              |
| --------------- | -------------------------- |
| `%ProgramData%` | `C:\ProgramData`           |
| `%SystemRoot%`  | `C:\Windows`               |
| `%TEMP%`        | `C:\Windows\TEMP` (system) |

Path separators: backslashes (`\`) are converted to forward slashes internally. Both formats work in the config file.

## Path-based matching with regex

Use match rules to apply different settings based on the request hostname or URL path. Match rules use [RE2 regular expressions](https://github.com/google/re2/wiki/Syntax).

Write a rule as `hostname:` or `path:` followed directly by the regex, with no space after the colon: everything after the colon up to the end of the line, spaces included, is the regex. The regex must match the whole value (an RE2 full match), so `path:^/blog/` matches only the exact path `/blog/`; end a prefix rule with `.*` to match everything under it. `hostname` is matched against the request's host name and `path` against the URL path without the query string. Trailing spaces are part of the regex too, so a rule that ends in `$ ` never matches. The host name is lowercased before it is matched, so write a `hostname` pattern in lowercase or prefix it with `(?i)`.

### hostname

Match by hostname:

```text
# Only optimize requests for this domain
hostname:^www\.example\.com$

pagespeed on
pagespeed EnableFilters rewrite_images,rewrite_css
```

### path

Match by URL path:

```text
# Aggressive optimization for the blog
path:^/blog/.*

pagespeed EnableFilters prioritize_critical_css,defer_javascript

# Conservative optimization for the checkout
path:^/checkout/.*

pagespeed RewriteLevel OptimizeForBandwidth
```

### Combining match rules

Match rules apply sequentially. Each rule sets a match context; directives following a rule apply only when that rule matches. A new match rule starts a new context.

```text
# Server-wide defaults
pagespeed on
pagespeed RewriteLevel CoreFilters

# Site A: full optimization
hostname:^site-a\.example\.com$
pagespeed EnableFilters collapse_whitespace,remove_comments

# Site B: bandwidth-only optimization
hostname:^site-b\.example\.com$
pagespeed RewriteLevel OptimizeForBandwidth

# Blog section on any site: add critical CSS
path:^/blog/.*
pagespeed EnableFilters prioritize_critical_css
```

### Clearing inherited settings

Use `clear` to reset accumulated options before applying new ones:

```text
hostname:^special\.example\.com$
clear
pagespeed on
pagespeed RewriteLevel PassThrough
pagespeed EnableFilters rewrite_images
```

The `clear` directive discards all previously accumulated options for this request, starting fresh with only the directives that follow.

## Custom fetch headers

Add custom headers to resource fetch requests using the `header_` prefix:

```text
pagespeed header_X-PageSpeed-Fetch true
pagespeed header_Authorization "Bearer token123"
```

The `header_` prefix is stripped; the remaining text becomes the header name.

## Complete examples

### Minimal configuration

```text
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
```

### Production configuration

```text
# Enable optimization with CoreFilters
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
pagespeed FileCacheSizeKb 2097152

# Add image optimization and critical CSS
pagespeed EnableFilters prioritize_critical_css
pagespeed EnableFilters lazyload_images

# Disable combining (breaks some sites)
pagespeed DisableFilters combine_css,combine_javascript

# HTTPS resource fetching
pagespeed FetchHttps enable

# Admin interface (local access only)
pagespeed AdminPath /pagespeed_admin
pagespeed GlobalAdminPath /pagespeed_global_admin
pagespeed MessageBufferSize 100000
pagespeed StatisticsLogging on
pagespeed LogDir %ProgramData%\We-Amp\PageSpeed\Logs
```

### Multi-site configuration

```text
# Server-wide defaults
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
pagespeed RewriteLevel CoreFilters

# Marketing site: aggressive optimization
hostname:^www\.example\.com$
pagespeed EnableFilters prioritize_critical_css,defer_javascript
pagespeed EnableFilters lazyload_images,collapse_whitespace

# API: no HTML optimization, bandwidth only
hostname:^api\.example\.com$
pagespeed RewriteLevel OptimizeForBandwidth

# Admin portal: disable optimization
hostname:^admin\.example\.com$
pagespeed off
```

## Configuration reload

The module checks the modification timestamp of `pagespeed.config` periodically (every 1000 ms). When a change is detected, the configuration is reloaded automatically without restarting IIS or recycling the app pool.

## See also

- [Configuration](/docs/configuration/) — general configuration reference (all platforms)
- [Filter selection](/docs/filter-selection/) — choosing and tuning filters
- [IIS Tuning](/docs/iis-configuration/#iis-tuning) — IIS-specific web server tuning
- [Install on IIS](/docs/install-iis/) — installation guide

## IIS tuning

IIS has its own performance controls that sit alongside mod_pagespeed. Configure them through IIS Manager or `web.config` — not through the [`pagespeed.config` file](/docs/iis-configuration/) that controls mod_pagespeed itself. This guide covers the settings worth changing when mod_pagespeed is active: request filtering for combined resources, gzip compression, CDN integration, Application Request Routing, and TCP slow start.

### Request filtering for combined resources

If you enable the [`combine_css`](/docs/css-filters/#combine_css) or [`combine_javascript`](/docs/javascript-filters/#combine_javascript) filters, the combined resource URLs contain a `+` character — for example `a.css+b.css.pagespeed.cc.HASH.css`. IIS request filtering rejects these as double-escaped and returns **HTTP 404.11** unless you allow them in the site's `web.config`:

```xml
<system.webServer>
  <security>
    <requestFiltering allowDoubleEscaping="true" />
  </security>
</system.webServer>
```

The installer ships this snippet as a `web.config.sample` file in the installation folder. If you do not use the combine filters, this setting is not required.

### Gzip compression

#### Static vs dynamic compression

IIS supports two types of gzip compression: static (pre-compressed files cached to disk) and dynamic (compressed on the fly). When mod_pagespeed is active, disable static compression and enable dynamic compression:

```xml
<system.webServer>
  <urlCompression doStaticCompression="false" doDynamicCompression="true" />
</system.webServer>
```

**Why disable static compression?** mod_pagespeed rewrites resource URLs with content hashes. Static compression caches compressed files by URL, but mod_pagespeed's URL rewriting means the compressed cache is rarely hit. Dynamic compression handles the constantly changing URLs more efficiently.

#### Gzip MIME types

Add MIME types for file formats that benefit from compression but are not compressed by IIS by default:

```xml
<system.webServer>
  <staticContent>
    <remove fileExtension=".woff" />
    <mimeMap fileExtension=".woff" mimeType="application/x-woff" />
    <remove fileExtension=".svg" />
    <mimeMap fileExtension=".svg" mimeType="image/svg+xml" />
    <remove fileExtension=".svgz" />
    <mimeMap fileExtension=".svgz" mimeType="image/svg+xml" />
    <remove fileExtension=".json" />
    <mimeMap fileExtension=".json" mimeType="application/json" />
  </staticContent>
</system.webServer>
```

### CDN integration

#### Gzip through proxies

By default, IIS disables compression for HTTP/1.0 clients and proxied requests. CDNs typically use HTTP/1.1 but may present a `Via` header that triggers IIS's proxy detection. Override this behavior:

```xml
<system.webServer>
  <httpCompression noCompressionForHttp10="false" noCompressionForProxies="false" />
</system.webServer>
```

#### CORS headers for CDN-served fonts

If your CDN serves web fonts from a different domain, browsers require CORS headers. Add the `Access-Control-Allow-Origin` header:

```xml
<system.webServer>
  <httpProtocol>
    <customHeaders>
      <add name="Access-Control-Allow-Origin" value="*" />
    </customHeaders>
  </httpProtocol>
</system.webServer>
```

Restrict the `value` to your specific domain instead of `*` if your security policy requires it.

### Application Request Routing (ARR)

ARR turns IIS into a reverse proxy with disk-based caching. When combined with mod_pagespeed, ARR can cache optimized resources at the edge and reduce load on the backend.

#### How it works with mod_pagespeed

1. mod_pagespeed's [`extend_cache`](/docs/cache-control/#extend_cache) filter rewrites cacheable resource URLs to include content hashes, extending their cache lifetime to one year.
2. ARR caches these long-lived resources. After the first request, subsequent requests are served directly from ARR's disk cache without reaching the backend.
3. The backend handles only dynamic HTML responses and cache misses.

#### Setup

Download ARR from Microsoft. Key configuration:

- Enable disk caching in ARR's proxy settings
- Set the cache drive to an SSD for best performance
- Configure the backend server farm to point to your mod_pagespeed-enabled application servers

ARR respects standard `Cache-Control` headers. mod_pagespeed sets appropriate caching headers on optimized resources automatically.

### TCP slow start

TCP slow start controls how quickly a new connection ramps up its sending rate. A larger initial congestion window (ICW) allows more data in the first round trip, which benefits page load times.

All supported Windows Server versions (2016 and later) default to an initial congestion window of 10 (ICW10). No configuration change is needed.

### See also

- [Install on IIS](/docs/install-iis/) — IIS installation guide
- [Configuration](/docs/configuration/) — general configuration reference
- [IIS configuration](/docs/iis-configuration/) — pagespeed.config format reference

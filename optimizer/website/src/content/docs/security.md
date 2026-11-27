---
title: 'Security hardening guide'
description: 'Security guidance for mod_pagespeed: restricting admin pages, domain authorization, untrusted content, CSS/XSS, cache poisoning, HTTPS, CVE patching.'
order: 57
group: 'Operate'
lastUpdated: 2026-10-22
---

## Overview

mod_pagespeed rewrites your HTML and the resources it references. That rewriting touches a few areas with security implications in production: admin-page access, domain authorization, and the handling of untrusted content. Each is covered below.

## Restrict admin page access

The admin pages at `/pagespeed_admin/` can purge caches and reveal configuration details. The process-wide `/pagespeed_global_admin/` endpoint is at least as sensitive. Always restrict access in production. See [Admin console](/docs/admin-console/) for setup.

### Hardening admin endpoints

Gate `/pagespeed_admin` and `/pagespeed_global_admin` where the web server itself decides which page a request reaches — `location =` / `^~` on nginx, `<Location>` on Apache, the module's `InfoUrlsLocalOnly` check on IIS. A WAF or upstream filter that matches the text of the URL can be bypassed. See [Admin Console — URL-path ACLs are brittle](/docs/admin-console/#url-path-acls-are-brittle).

On nginx, the admin, statistics, console and message pages are restricted with `location` blocks, and every `server` block that serves one of these pages needs them. A path set in the `http` block — `GlobalAdminPath` and `GlobalStatisticsPath` can only be set there — is served by every `server` block, including the default server. After updating from 1.16.0 or earlier, replace your access rules for these pages with the example under [Admin Console — Setup](/docs/admin-console/#setup), in every `server` block; the update does not change rules that are already in your configuration.

## Domain authorization

mod_pagespeed only fetches resources from explicitly authorized domains. This prevents it from being used as an open proxy. Authorize only domains you control. See [Domain configuration](/docs/domain-configuration/).

## Untrusted content

mod_pagespeed rewrites URLs and inlines content. If your site serves user-generated HTML (forums, CMSes), consider:

- Inline filters (`inline_css`, `inline_javascript`) will inline resources from authorized domains into the page. If a user can inject `<link>` or `<script>` tags pointing to authorized domains, the inlined content appears in the HTML response.
- Use `ForbidFilters` to disable inlining in sections serving untrusted content.

## CSS and XSS

CSS can contain `url()` references and, in older browsers, expressions. mod_pagespeed's CSS rewriting preserves these constructs. If untrusted users can inject CSS into your pages, the rewritten CSS will still contain the injected content.

## Cache poisoning

mod_pagespeed caches optimized resources keyed by URL. If an attacker can manipulate request parameters that affect page content (but not the cache key), they could potentially poison the cache. mod_pagespeed mitigates this by including relevant request properties in cache keys.

## HTTPS

mod_pagespeed verifies SSL certificates when fetching HTTPS resources. Do not disable certificate verification in production. See [HTTPS configuration](/docs/https-configuration/).

## Security patches

mod_pagespeed includes patches for all known CVEs from the open-source project. We-Amp maintains an ongoing security review process. If you discover a security issue, report it to [security@modpagespeed.com](mailto:security@modpagespeed.com) or through the [security policy](/security/).

## See also

- [Admin console](/docs/admin-console/) — restricting admin access
- [Domain configuration](/docs/domain-configuration/) — domain authorization
- [HTTPS configuration](/docs/https-configuration/) — SSL/TLS setup
- [Configuring Content-Security-Policy with mod_pagespeed](/blog/mod-pagespeed-content-security-policy/) — keeping optimization CSP-safe

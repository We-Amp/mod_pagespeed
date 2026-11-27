---
title: 'Filter selection'
description: 'Choose which mod_pagespeed filters run: RewriteLevel presets, the CoreFilters default, EnableFilters, DisableFilters, ForbidFilters and thresholds.'
order: 41
group: 'Filters'
lastUpdated: 2026-10-22
---

mod_pagespeed applies a set of filters to optimize your pages. You control which filters run through the `RewriteLevel` directive and per-filter enable/disable directives.

For what each filter does, see the [PageSpeed filters](/docs/filters/) table. This page covers how to select them. For where the filters fit in the wider work, and what no filter can do, see [PageSpeed optimization](/pagespeed-optimization/).

## RewriteLevel

The `RewriteLevel` directive sets the baseline set of filters. Three levels are available:

### CoreFilters (default)

A production-safe set of filters enabled by default. This level balances optimization impact with broad compatibility. Most deployments should start here and add or remove individual filters as needed.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed RewriteLevel CoreFilters;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedRewriteLevel CoreFilters
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed RewriteLevel CoreFilters
```

</div>

### PassThrough

No filters are enabled by default. Use this level when you want full control over exactly which filters run. You then enable individual filters with `EnableFilters`.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed RewriteLevel PassThrough;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedRewriteLevel PassThrough
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed RewriteLevel PassThrough
```

</div>

### OptimizeForBandwidth

Optimizes resources in place without altering HTML structure. Images are recompressed, CSS and JavaScript are minified, but no URLs are rewritten and no HTML is changed. This level is safe for environments where URL stability is critical.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed RewriteLevel OptimizeForBandwidth;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedRewriteLevel OptimizeForBandwidth
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed RewriteLevel OptimizeForBandwidth
```

</div>

## CoreFilters

When `RewriteLevel` is set to `CoreFilters` (the default), the following filters are enabled:

- [`add_head`](/docs/html-filters/#add_head) — adds a `<head>` element if one is missing
- [`combine_css`](/docs/css-filters/#combine_css) — combines multiple CSS files into one
- [`combine_javascript`](/docs/javascript-filters/#combine_javascript) — combines multiple JavaScript files into one
- [`convert_meta_tags`](/docs/html-filters/#convert_meta_tags) — converts `<meta http-equiv>` tags to response headers
- [`extend_cache`](/docs/cache-control/#extend_cache) — extends cache lifetime of resources by content-hashing URLs
- [`fallback_rewrite_css_urls`](/docs/css-filters/#fallback_rewrite_css_urls) — rewrites URLs in CSS even when CSS parsing fails
- [`flatten_css_imports`](/docs/css-filters/#flatten_css_imports) — inlines `@import` rules in CSS
- [`inline_css`](/docs/css-filters/#inline_css) — inlines small CSS files into HTML
- [`inline_import_to_link`](/docs/css-filters/#inline_import_to_link) — converts CSS `@import` to `<link>` tags
- [`inline_javascript`](/docs/javascript-filters/#inline_javascript) — inlines small JavaScript files into HTML
- [`rewrite_css`](/docs/css-filters/#rewrite_css) — minifies CSS
- [`rewrite_images`](/docs/image-filters/#rewrite_images) — recompresses and resizes images
- [`rewrite_javascript`](/docs/javascript-filters/#rewrite_javascript) — minifies JavaScript
- [`rewrite_style_attributes_with_url`](/docs/css-filters/#rewrite_style_attributes) — rewrites URLs in `style` attributes

## Enabling and disabling filters

### EnableFilters

Adds filters on top of those already active from the RewriteLevel. Accepts a comma-separated list of filter names.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed EnableFilters collapse_whitespace,remove_comments;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedEnableFilters collapse_whitespace,remove_comments
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed EnableFilters collapse_whitespace,remove_comments
```

</div>

### DisableFilters

Removes filters from the active set. Use this to turn off specific CoreFilters you do not want.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed DisableFilters combine_css,combine_javascript;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedDisableFilters combine_css,combine_javascript
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed DisableFilters combine_css,combine_javascript
```

</div>

### ForbidFilters

Blocks filters from being enabled by any means, including query parameters and `.htaccess` overrides. Use this to lock down your configuration in multi-tenant or shared-hosting environments.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed ForbidFilters inline_javascript,inline_css;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedForbidFilters inline_javascript,inline_css
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed ForbidFilters inline_javascript,inline_css
```

</div>

### ForbidAllDisabledFilters

Automatically forbids every filter that is not currently enabled. This prevents users from enabling additional filters via query parameters or `.htaccess`.

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed ForbidAllDisabledFilters on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedForbidAllDisabledFilters on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed ForbidAllDisabledFilters on
```

</div>

On pages mod_pagespeed recognizes as hydrated, some filters do not run. See
[Hydrated pages](/docs/hydrated-pages/).

## Checking your filter configuration

Open the admin page at `/pagespeed_admin/config` in your browser to see the full list of active filters and their current settings. This page reflects the running configuration after all directives are applied.

## Tuning parameters

Several directives control size thresholds and quality levels for filter behavior. The table below lists commonly adjusted parameters with their defaults.

| Parameter                   | Default | Description                                                                                |
| --------------------------- | ------- | ------------------------------------------------------------------------------------------ |
| `CssInlineMaxBytes`         | 2048    | Maximum size (bytes) of a CSS file to inline into HTML                                     |
| `JsInlineMaxBytes`          | 2048    | Maximum size (bytes) of a JavaScript file to inline into HTML                              |
| `ImageInlineMaxBytes`       | 3072    | Maximum size (bytes) of an image to inline as a data URI                                   |
| `ImageRecompressionQuality` | 85      | Quality level (-1 to 100; -1 uses the source image's quality) for recompressed JPEG images |
| `CssFlattenMaxBytes`        | 1024000 | Maximum size (bytes) of CSS after flattening `@import` rules                               |

Since v1.15.0+r18, out-of-range values for bounded parameters fail configuration load instead of being silently clamped, so check ranges when tuning.

Set these in your server configuration:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed CssInlineMaxBytes 4096;
pagespeed ImageRecompressionQuality 75;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedCssInlineMaxBytes 4096
ModPagespeedImageRecompressionQuality 75
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed CssInlineMaxBytes 4096
pagespeed ImageRecompressionQuality 75
```

</div>

## URL preservation

By default, mod_pagespeed rewrites resource URLs to include content hashes for cache extension. If you use a CDN or other infrastructure that requires stable, unchanged URLs, set the URL-preservation options for the affected resource types:

<!-- platform: nginx -->

<div data-platform="nginx" data-platform-label="nginx">

```nginx
pagespeed ImagePreserveURLs on;
pagespeed CssPreserveURLs on;
pagespeed JsPreserveURLs on;
```

</div>

<!-- platform: apache -->

<div data-platform="apache" data-platform-label="Apache">

```apache
ModPagespeedImagePreserveURLs on
ModPagespeedCssPreserveURLs on
ModPagespeedJsPreserveURLs on
```

</div>

<!-- platform: iis -->

<div data-platform="iis" data-platform-label="IIS">

```text
pagespeed ImagePreserveURLs on
pagespeed CssPreserveURLs on
pagespeed JsPreserveURLs on
```

</div>

With URL preservation enabled, mod_pagespeed still optimizes the resource content (recompression, minification) but serves it at the original URL. The optimized variant is loaded on the first request and served on subsequent requests via the in-place resource optimization flow.

## Links

- [PageSpeed filters](/docs/filters/) — every filter, what it does and which level enables it
- [Configuration](/docs/configuration/) — general configuration directives

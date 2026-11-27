---
title: 'Hydrated pages'
description: 'How mod_pagespeed handles hydrated React, Next.js, Vue, Nuxt and Svelte pages, what to do when recognition misses one, and the DetectHydratedPages option.'
order: 48
group: 'Filters'
lastUpdated: 2026-11-02
---

## Overview

Server-rendered React, Next.js, Vue, Nuxt and Svelte pages no longer break
hydration because of mod_pagespeed's HTML rewrites. On a page mod_pagespeed
recognizes as hydrated, stylesheet and script URLs, inline styles, comments,
whitespace and script deferral are left unchanged and images are still
optimized, so CSS and script savings may drop on those pages.

## What is left unchanged

On a page mod_pagespeed recognizes as hydrated:

- Stylesheet and script URLs are left unchanged.
- Inline styles are left unchanged.
- Comments are left unchanged.
- Whitespace is left unchanged.
- Script deferral is left unchanged.

mod_pagespeed still optimizes images on these pages. CSS and script savings
may drop on those pages.

## The filters that do not run

These filters do not run on a page mod_pagespeed recognizes as hydrated:

- **CSS:** [`rewrite_css`](/docs/filters/rewrite_css/), [`flatten_css_imports`](/docs/filters/flatten_css_imports/), [`rewrite_style_attributes`](/docs/filters/rewrite_style_attributes/), [`rewrite_style_attributes_with_url`](/docs/filters/rewrite_style_attributes_with_url/), [`fallback_rewrite_css_urls`](/docs/filters/fallback_rewrite_css_urls/), [`combine_css`](/docs/filters/combine_css/), [`inline_css`](/docs/filters/inline_css/), [`inline_google_font_css`](/docs/filters/inline_google_font_css/), [`inline_import_to_link`](/docs/filters/inline_import_to_link/), [`outline_css`](/docs/filters/outline_css/), [`move_css_to_head`](/docs/filters/move_css_to_head/), [`move_css_above_scripts`](/docs/filters/move_css_above_scripts/), [`prioritize_critical_css`](/docs/filters/prioritize_critical_css/)
- **JavaScript:** [`rewrite_javascript`](/docs/filters/rewrite_javascript/), [`combine_javascript`](/docs/filters/combine_javascript/), [`inline_javascript`](/docs/filters/inline_javascript/), [`outline_javascript`](/docs/filters/outline_javascript/)
- **HTML:** [`remove_comments`](/docs/filters/remove_comments/), [`collapse_whitespace`](/docs/filters/collapse_whitespace/)

[`defer_javascript`](/docs/filters/defer_javascript/) does not run on these
pages either.

[`extend_cache`](/docs/filters/extend_cache/), [`rewrite_domains`](/docs/filters/rewrite_domains/)
and [`trim_urls`](/docs/filters/trim_urls/) skip stylesheets and scripts on
these pages.

The critical image beacon is not added on these pages, because its script
would land inside the framework's markup. Preload hints for stylesheets and
scripts stay; image preload hints are removed as on any other page, since the
images they name are rewritten.

## If recognition misses a page

Recognition can miss a page. On such a page, disable the listed filters with
[`DisableFilters`](/docs/filter-selection/#disablefilters) and also set
[`CriticalImagesBeaconEnabled`](/docs/configuration/#criticalimagesbeaconenabled)
to `false`:

**Apache:**

```apache
ModPagespeedDisableFilters rewrite_css,flatten_css_imports,rewrite_style_attributes,rewrite_style_attributes_with_url,fallback_rewrite_css_urls,combine_css,inline_css,inline_google_font_css,inline_import_to_link,outline_css,move_css_to_head,move_css_above_scripts,prioritize_critical_css,rewrite_javascript,combine_javascript,inline_javascript,outline_javascript,remove_comments,collapse_whitespace
ModPagespeedCriticalImagesBeaconEnabled false
```

**Nginx:**

```nginx
pagespeed DisableFilters rewrite_css,flatten_css_imports,rewrite_style_attributes,rewrite_style_attributes_with_url,fallback_rewrite_css_urls,combine_css,inline_css,inline_google_font_css,inline_import_to_link,outline_css,move_css_to_head,move_css_above_scripts,prioritize_critical_css,rewrite_javascript,combine_javascript,inline_javascript,outline_javascript,remove_comments,collapse_whitespace;
pagespeed CriticalImagesBeaconEnabled false;
```

On IIS, use the same filter names with the `pagespeed` prefix in
`pagespeed.config` (no semicolons):

```text
pagespeed DisableFilters rewrite_css,flatten_css_imports,rewrite_style_attributes,rewrite_style_attributes_with_url,fallback_rewrite_css_urls,combine_css,inline_css,inline_google_font_css,inline_import_to_link,outline_css,move_css_to_head,move_css_above_scripts,prioritize_critical_css,rewrite_javascript,combine_javascript,inline_javascript,outline_javascript,remove_comments,collapse_whitespace
pagespeed CriticalImagesBeaconEnabled false
```

See [IIS configuration](/docs/iis-configuration/) for the full file format
reference.

## After upgrading

Purge cached HTML in any CDN after upgrading, because pages rewritten by an
earlier version can still carry the changed stylesheet and script URLs that
make hydration fall back or load a second copy. See
[CDN integration](/docs/cdn-integration/) for CDN-level caching guidance.

## DetectHydratedPages

A server-wide option, `DetectHydratedPages`, turns recognition off. The
directive is rejected inside a virtual host or server block, so set it in the
server-wide configuration.

On Apache and nginx, use one of the forms below. On IIS, set it in the
machine-wide `pagespeed.config`; see [IIS configuration](/docs/iis-configuration/).

```apache
# Apache, global server configuration
ModPagespeedDetectHydratedPages off
```

```nginx
# nginx, http block
pagespeed DetectHydratedPages off;
```

## See also

- [Filter selection](/docs/filter-selection/)
- [CSS filters](/docs/css-filters/)
- [JavaScript filters](/docs/javascript-filters/)
- [HTML filters](/docs/html-filters/)
- [CDN integration](/docs/cdn-integration/)

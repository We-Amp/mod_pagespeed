---
title: 'CSS filters'
description: 'CSS filters in mod_pagespeed: minify, combine, inline and flatten @import CSS, plus critical-CSS extraction. Apache, nginx and IIS syntax with tuning.'
order: 44
group: 'Filters'
lastUpdated: 2026-10-22
---

## Overview

mod_pagespeed includes filters for CSS minification, combining, inlining, import flattening, and critical CSS extraction. Several CSS filters are CoreFilters and run by default. The remaining filters can be enabled individually or through OptimizeForBandwidth mode.

On pages mod_pagespeed recognizes as hydrated, some of these filters do not run. See [Hydrated pages](/docs/hydrated-pages/).

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```text
pagespeed EnableFilters rewrite_css,combine_css
pagespeed CssInlineMaxBytes 4096
```

See [IIS configuration](/docs/iis-configuration/) for the full file format reference.

## Quick reference

| Filter                                                           | Core | OFB | Description                                           | Safe           |
| ---------------------------------------------------------------- | ---- | --- | ----------------------------------------------------- | -------------- |
| [`rewrite_css`](#rewrite_css)                                    | Yes  | Yes | Minifies CSS and rewrites embedded URLs               | Generally safe |
| [`fallback_rewrite_css_urls`](#fallback_rewrite_css_urls)        | Yes  | No  | Rewrites URLs in unparseable CSS                      | Generally safe |
| [`rewrite_style_attributes`](#rewrite_style_attributes)          | No   | No  | Rewrites inline `style` attributes                    | Generally safe |
| [`rewrite_style_attributes_with_url`](#rewrite_style_attributes) | Yes  | No  | Rewrites inline `style` attributes containing `url()` | Generally safe |
| [`combine_css`](#combine_css)                                    | Yes  | No  | Combines multiple CSS files into one                  | Generally safe |
| [`flatten_css_imports`](#flatten_css_imports)                    | Yes  | No  | Inlines `@import` rules                               | Generally safe |
| [`inline_css`](#inline_css)                                      | Yes  | No  | Inlines small external CSS into HTML                  | Generally safe |
| [`inline_import_to_link`](#inline_import_to_link)                | Yes  | No  | Converts `@import` in `<style>` to `<link>`           | Generally safe |
| [`inline_google_font_css`](#inline_google_font_css)              | No   | No  | Inlines Google Fonts CSS                              | Generally safe |
| [`outline_css`](#outline_css)                                    | No   | No  | Externalizes large inline CSS                         | Experimental   |
| [`prioritize_critical_css`](#prioritize_critical_css)            | No   | No  | Inlines the CSS a page uses, loads the rest async     | Test first     |
| [`move_css_above_scripts`](#move_css_above_scripts)              | No   | No  | Moves CSS `<link>` above `<script>` elements          | Generally safe |
| [`move_css_to_head`](#move_css_to_head)                          | No   | No  | Moves CSS `<link>` elements into `<head>`             | Generally safe |
| [`extend_cache_css`](#extend_cache_css)                          | Yes  | No  | Content-hashed stylesheet URLs with a one-year cache  | Generally safe |
| [`compute_critical_css`](#compute_critical_css)                  | No   | No  | Background critical-CSS computation (experimental)    | Experimental   |

## Filter details

### rewrite_css {#rewrite_css}

[Full guide →](/docs/filters/rewrite_css/)

#### What it does

`rewrite_css` parses each stylesheet, minifies it, and rewrites the `url()` references inside it so images and fonts go through mod_pagespeed's optimization and cache-extension pipeline. The result is served from a rewritten `.pagespeed.cf.` URL with a long cache lifetime; the original file on disk is never touched. In OptimizeForBandwidth mode the minified bytes replace the original response in place and the URL stays as authored. Live demo: [rewrite_css](/examples/rewrite_css/).

```text
/* before */
/* Site header, see ticket 412 */
.header {
  margin: 0px 0px 16px 0px;
  background: #ffffff url(/img/banner.png) no-repeat;
}

/* after */
.header{margin:0 0 16px 0;background:#fff url(/img/banner.png.pagespeed.ce.HASH.png) no-repeat}
```

#### When it helps and when it does not

Minification helps most on hand-maintained CSS with comments and generous formatting, and the URL rewriting helps wherever stylesheet-referenced images are not already optimized. A build pipeline that already minifies and fingerprints its CSS leaves the filter little to do; running it anyway only adds a rewrite step. Because the parser declines a stylesheet it cannot fully parse rather than guessing, heavily hack-laden legacy CSS may pass through unminified — the companion `fallback_rewrite_css_urls` still rewrites the URLs inside such files.

#### How it decides

The filter runs a real CSS parser over the file. On a parse failure it produces no minified output and leaves minification to no one: only URL rewriting can still happen, through `fallback_rewrite_css_urls`. Values are shortened only where the equivalence is exact, such as `0px` to `0` and `#ffffff` to `#fff`. The stylesheet must sit on a domain the module is authorized to fetch, and the rewritten URL embeds a content hash so a changed file is picked up without a purge.

#### Risks

- A stylesheet that relies on parser-error recovery (old browser hacks) can be declined or rewritten differently than a browser would interpret it; test such files before rolling out.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-rewrite_css` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters rewrite_css
```

```nginx
# Nginx
pagespeed EnableFilters rewrite_css;
```

### fallback_rewrite_css_urls {#fallback_rewrite_css_urls}

[Full guide →](/docs/filters/fallback_rewrite_css_urls/)

Core filter. Rewrites resource URLs embedded in CSS files even when CSS parsing fails. Acts as a safety net for non-standard CSS that the full parser cannot handle.

Enable:

```apache
# Apache
ModPagespeedEnableFilters fallback_rewrite_css_urls
```

```nginx
# Nginx
pagespeed EnableFilters fallback_rewrite_css_urls;
```

### rewrite_style_attributes / rewrite_style_attributes_with_url {#rewrite_style_attributes}

<a id="rewrite_style_attributes_with_url"></a>

[Full guide →](/docs/filters/rewrite_style_attributes/) · Also: [`rewrite_style_attributes_with_url`](/docs/filters/rewrite_style_attributes_with_url/)

`rewrite_style_attributes` applies CSS rewriting (minification, URL rewriting) to inline `style=""` attributes on HTML elements. `rewrite_style_attributes_with_url` (Core filter) does the same but only when the style value contains a `url()` reference.

Enable:

```apache
# Apache
ModPagespeedEnableFilters rewrite_style_attributes
# or, for URL-only (enabled by default as a CoreFilter):
ModPagespeedEnableFilters rewrite_style_attributes_with_url
```

```nginx
# Nginx
pagespeed EnableFilters rewrite_style_attributes;
# or, for URL-only (enabled by default as a CoreFilter):
pagespeed EnableFilters rewrite_style_attributes_with_url;
```

### combine_css {#combine_css}

[Full guide →](/docs/filters/combine_css/)

#### What it does

`combine_css` concatenates consecutive `<link rel="stylesheet">` references into one stylesheet and swaps the group for a single `<link>` to the combined file at a `.pagespeed.cc.` URL. Three stylesheets in a row become one request, and the rules keep their original order so the cascade is unchanged. Live demo: [combine_css](/examples/combine_css/).

```html
<!-- before -->
<link rel="stylesheet" href="/css/reset.css" />
<link rel="stylesheet" href="/css/layout.css" />
<link rel="stylesheet" href="/css/theme.css" />

<!-- after -->
<link rel="stylesheet" href="/css/reset.css+layout.css+theme.css.pagespeed.cc.HASH.css" />
```

#### When it helps and when it does not

Combining was designed for HTTP/1.1 connection limits. CSS is render-blocking, so even over HTTP/2 one combined fetch can beat several discovered-in-parallel fetches, but the margin is much smaller than it was: multiplexing removes the queueing that made combining essential. The counter-costs are real: the combined file invalidates as a whole when any member changes, and first paint waits for the entire combined download. Sites that already ship one bundled stylesheet gain nothing.

#### How it decides

Only consecutive links with the same `media` value combine; a different `media` attribute starts a new group, as does an inline `<style>` block, an IE conditional comment, a `<link>` inside `<noscript>`, or a link carrying extra attributes such as `id` or `title`, which is left as authored. Every member must come from a domain the module is authorized to fetch. `MaxCombinedCssBytes` caps the combined size and defaults to -1, no limit.

#### Risks

- Relative `url()` paths inside combined files are resolved against each member's own location, so members from different directories combine safely; what does not survive is markup that depends on the exact set of `<link>` elements, such as scripts that toggle stylesheets by index.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-combine_css` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters combine_css
ModPagespeedMaxCombinedCssBytes 102400
```

```nginx
# Nginx
pagespeed EnableFilters combine_css;
pagespeed MaxCombinedCssBytes 102400;
```

### flatten_css_imports {#flatten_css_imports}

[Full guide →](/docs/filters/flatten_css_imports/)

Core filter. Replaces CSS `@import` rules with the contents of the imported file. Eliminates round trips caused by import chains. The `CssFlattenMaxBytes` parameter (default: 1024000) limits the size of the resulting flattened CSS. Flattening is trickier than plain concatenation — media queries, charset rules, and relative URLs all have to survive the merge; see [flattening CSS @imports](/blog/flatten-css-imports-edge-cases/) for the edge cases.

Enable:

```apache
# Apache
ModPagespeedEnableFilters flatten_css_imports
```

```nginx
# Nginx
pagespeed EnableFilters flatten_css_imports;
```

### inline_css {#inline_css}

[Full guide →](/docs/filters/inline_css/)

#### What it does

`inline_css` replaces a small external stylesheet with an inline `<style>` block holding the file's contents, so first paint no longer waits on that fetch. Relative `url()` paths inside the stylesheet are made absolute first, so images and fonts keep resolving from the page's location. Live demo: [inline_css](/examples/inline_css/).

```text
<!-- before: a render-blocking request for a 1.8 KB file -->
<link rel="stylesheet" href="/css/header.css">

<!-- after: the rules sit inside the page -->
<style>.site-header{display:flex;gap:1rem}.site-header img{height:2rem}</style>
```

#### When it helps and when it does not

CSS is render-blocking, so for a tiny stylesheet the removed round trip is worth more than the bytes: the request, its headers, and its connection setup all cost more than a kilobyte of inline text. The trade flips as files grow or get shared. An inlined stylesheet is not cached on its own, so it downloads again with every page view, and a file inlined into twenty pages is transferred twenty times. Stylesheets shared across many pages, and anything well above the threshold, are better left external with a long cache lifetime. `inline_css` is a CoreFilter, so the trade is already live on a default install.

#### How it decides

Only stylesheets whose contents are no larger than `CssInlineMaxBytes` (default 2048 bytes) qualify, and only files on domains the module is authorized to fetch. A stylesheet whose `media` attribute cannot affect the screen, such as `print`, stays external: inlining it would make every page pay for rules no screen visitor needs. A file that contains the text `</style>` stays external too, since it would end the inline block early. Nothing is inlined when the page's Content-Security-Policy forbids inline styles.

#### Risks

- The page grows by the stylesheet's size on every view, so keep `CssInlineMaxBytes` small. Raising it to inline a large file delays the HTML itself, which is worse than the fetch it removes.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-inline_css` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters inline_css
ModPagespeedCssInlineMaxBytes 2048
```

```nginx
# Nginx
pagespeed EnableFilters inline_css;
pagespeed CssInlineMaxBytes 2048;
```

### inline_import_to_link {#inline_import_to_link}

[Full guide →](/docs/filters/inline_import_to_link/)

Core filter. Converts `<style>@import url(...);</style>` to `<link rel="stylesheet">`, enabling other CSS filters (combining, minification) to process the imported stylesheet.

Enable:

```apache
# Apache
ModPagespeedEnableFilters inline_import_to_link
```

```nginx
# Nginx
pagespeed EnableFilters inline_import_to_link;
```

### inline_google_font_css {#inline_google_font_css}

[Full guide →](/docs/filters/inline_google_font_css/)

Not a core filter. Fetches the CSS from the Google Fonts API and inlines it directly into the HTML, eliminating one round trip. Requires HTTPS fetching to be enabled. GDPR considerations apply in the EU: inlining the CSS avoids the browser contacting Google Fonts servers directly, which can help with compliance.

Enable:

```apache
# Apache
ModPagespeedEnableFilters inline_google_font_css
```

```nginx
# Nginx
pagespeed EnableFilters inline_google_font_css;
```

### outline_css {#outline_css}

[Full guide →](/docs/filters/outline_css/)

#### What it does

`outline_css` is the inverse of `inline_css`: it takes a large inline `<style>` block out of the HTML and serves it as a stylesheet of its own, at a rewritten `_.pagespeed.co.` URL with a long cache lifetime, leaving a `<link rel="stylesheet">` in the block's place. Not a core filter; enable it by name. Live demo: [outline_css](/examples/outline_css/).

```html
<!-- before: 8 KB of rules ride inside every page response -->
<style type="text/css" id="large">
  .checkout { ... }
</style>

<!-- after: the rules are fetched once and kept in the browser cache -->
<link rel="stylesheet" href="/_.pagespeed.co.HASH.css" type="text/css" id="large" />
```

#### When it helps and when it does not

The trade is about which bytes repeat. A `<style>` block rides inside the HTML, so a template that pastes the same block into every page sends it again on each of them, and when the HTML is generated per request none of those bytes can be cached. Outlining gives the block a URL of its own: the browser fetches the `.pagespeed.co.` file once, keeps it in cache, and every later page in the same directory that carries the same block reuses it; the file is named at the page's own path, so pages in other directories get their own copy. What the first view pays is a render-blocking stylesheet request the page did not have before, and a block that only one page carries, or that differs from page to page, gets a file nobody else requests. Most sites are better served by keeping CSS in real files; this filter exists for HTML whose large stylesheets are stuck inline.

#### How it decides

Only a `<style>` block whose text is at least `CssOutlineMinBytes` (default 3000 bytes) is outlined. A `<style scoped>` element is left alone, because a scoped block cannot become a plain `<link>`, and so is a block whose `type` is anything other than CSS. Relative `url()` references inside the block are re-resolved against the generated file's location, so images keep loading from where they did. The generated `<link>` carries the original element's other attributes, so an `id` or `media` survives the move. The block must also arrive whole: when a flush lands in the middle of it, or a stray tag sits inside it, that block is not outlined and stays inline.

#### Risks

- A first-time visitor waits on a render-blocking stylesheet request the inline block did not have; on a page seen once, that wait costs more than the bytes it saves.
- Markup or scripts that expect the `<style>` element to exist in the page see a `<link>` instead.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-outline_css` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters outline_css
ModPagespeedCssOutlineMinBytes 3000
```

```nginx
# Nginx
pagespeed EnableFilters outline_css;
pagespeed CssOutlineMinBytes 3000;
```

### prioritize_critical_css {#prioritize_critical_css}

[Full guide →](/docs/filters/prioritize_critical_css/)

Not a core filter. Test before deploying. Inlines the CSS rules a page uses and loads each full stylesheet without blocking the first paint. The full stylesheet is preloaded from the place its `<link>` had in the page and takes effect there as soon as it has arrived, so the order in which your rules apply does not change; a `<noscript>` copy of the link covers visitors without scripts, and inline `<style>` blocks are left as they are. By default the inlined rules cover every element in the page as visitors' browsers last saw it, so content further down the page is styled from the first paint too. Uses a JavaScript beacon to collect critical CSS data from real user visits. The beacon endpoint must be accessible for data collection to work. Can cut perceived load time, but test it against your own page layouts first. In v1.15.0+r18 and later, the filter honors a restrictive `Content-Security-Policy` when `HonorCsp` is enabled: on pages whose policy disallows inline styles or scripts, it passes the page through unchanged instead of injecting content the policy would block. For the trade-offs behind critical-CSS extraction, see [how critical CSS is identified](/blog/critical-css-heuristics/).

Enable:

```apache
# Apache
ModPagespeedEnableFilters prioritize_critical_css
```

```nginx
# Nginx
pagespeed EnableFilters prioritize_critical_css;
```

If you prefer a smaller inline block and accept that content below the first screen may be partly styled until the full stylesheet arrives, turn on `CriticalCssAboveTheFoldOnly` (off by default): `ModPagespeedCriticalCssAboveTheFoldOnly on` on Apache, `pagespeed CriticalCssAboveTheFoldOnly on;` on nginx, `pagespeed CriticalCssAboveTheFoldOnly on` on IIS.

**What to expect.** After you deploy a changed stylesheet, the filter leaves that page's stylesheets blocking for a few page views, until visitors' browsers have reported on the new rules. After a page's markup changes without its stylesheets changing, rules that newly apply can be late until a visitor's browser reports on the new markup; the module asks for a report again after about a minute by default (twelve times `BeaconReinstrumentTimeSec`), and the wait is longer when the visitor who is asked does not report. The browser reports which rules the page uses once the page has loaded, so content that a script removes, hides or gives other class names before then can be painted without the rules that applied only to its earlier state, until the full stylesheet arrives. Content a script adds while the page is loading takes its rules from the full stylesheet. The full stylesheet is turned on by a small inline script: if something in front of your server delays inline scripts, the full styles arrive when that script runs. For pages whose markup differs from visitor to visitor under one URL, leave the filter off (`DisableFilters prioritize_critical_css` for that location).

A stylesheet keeps its ordinary blocking `<link>` when it uses an `@import` the server cannot merge into it, when nearly all of it would be inline anyway, or when its `<link>` carries an event-handler attribute, a `title` or `disabled`. A page with more matching selectors than one report can carry keeps its blocking stylesheets until a complete report arrives (see the `beacon_overflow_count` statistic). The filter needs the beacon and therefore does nothing on Envoy.

### move_css_above_scripts {#move_css_above_scripts}

[Full guide →](/docs/filters/move_css_above_scripts/)

#### What it does

`move_css_above_scripts` lifts stylesheet references that sit below the page's first `<script>` up to just before that first script. A browser will not run a script until the stylesheets above it have loaded, because the script may read layout; with stylesheets scattered around and below scripts, those downloads are discovered late and the script waits on them. Moving each stylesheet directly before the first script puts every download in front of the code that needs it. Not a core filter; enable it by name. Live demo: [move_css_above_scripts](/examples/move_css_above_scripts/).

```html
<!-- before -->
<script src="/js/theme.js"></script>
<link rel="stylesheet" href="/css/theme.css" />

<!-- after: the stylesheet loads first -->
<link rel="stylesheet" href="/css/theme.css" />
<script src="/js/theme.js"></script>
```

#### When it helps and when it does not

It helps on legacy or machine-generated pages where stylesheets ended up interleaved with or after scripts: template fragments that each carry their own `<link>`, or ad and widget snippets injecting CSS late in the body. On pages whose CSS already sits in `<head>` with scripts at the end of `<body>`, there is nothing after any script to move and the filter changes nothing. The stylesheets keep their order among themselves, so the cascade is unchanged; what changes is where the group sits relative to the script.

#### How it decides

The first `<script>` in the document is the anchor. Every `<style>` block and stylesheet `<link>` that appears after it moves to directly before that first script, in original order. When `move_css_to_head` is also enabled, whichever anchor closes first in the document, the `</head>` or the first script, wins and all moves go there. A stylesheet inside `<noscript>`, or a `<style scoped>` block, stays where it is and ends the current move; styles after it move up only as far as the next `<script>`.

#### Risks

- A stylesheet deliberately placed after a script by code that removes or replaces it at run time is moved too; such pages need the filter off for that path.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-move_css_above_scripts` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters move_css_above_scripts
```

```nginx
# Nginx
pagespeed EnableFilters move_css_above_scripts;
```

### move_css_to_head {#move_css_to_head}

[Full guide →](/docs/filters/move_css_to_head/)

#### What it does

`move_css_to_head` collects stylesheet references that sit in the `<body>` and appends them to the end of `<head>`. Stylesheets discovered in the body still apply, but the browser finds them late in the parse, and a late stylesheet can repaint content that was already shown without it. Gathering them into `<head>` puts every download where the browser expects stylesheets and starts them all at once. Not a core filter; enable it by name. Live demo: [move_css_to_head](/examples/move_css_to_head/).

```html
<!-- before -->
<head>
  ...no stylesheet for comments...
</head>
<body>
  <article>...</article>
  <link rel="stylesheet" href="/css/comments.css" />
</body>

<!-- after: the link is appended to the end of head -->
<head>
  ...
  <link rel="stylesheet" href="/css/comments.css" />
</head>
<body>
  <article>...</article>
</body>
```

#### When it helps and when it does not

It helps on pages whose markup carries `<link>` or `<style>` elements inside the body, which is typical of older templates and CMS output that renders per-section stylesheets where the section appears. On pages that already keep their CSS in `<head>` there is nothing to move and the filter is a no-op. The filter changes where stylesheets load from, not their order relative to each other, so the cascade survives; the visible difference is that it removes the repaint a late stylesheet would cause.

#### How it decides

The first `</head>` is the anchor, and every `<style>` block and stylesheet `<link>` after it moves to the end of `<head>`, in original order. A stylesheet inside `<noscript>`, or a `<style scoped>` block, stays where the author put it, and styles after it are not moved either. When `move_css_above_scripts` is also enabled, the first anchor in the document, the `</head>` or the first `<script>`, decides where the styles go.

#### Risks

- Stylesheets a script deliberately placed in the body, for example one that swaps a `<link>` after load, are moved as well; disable the filter for such pages.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-move_css_to_head` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters move_css_to_head
```

```nginx
# Nginx
pagespeed EnableFilters move_css_to_head;
```

### extend_cache_css {#extend_cache_css}

[Full guide →](/docs/filters/extend_cache_css/)

Core filter, one of the three members of [`extend_cache`](/docs/cache-control/#extend_cache). Rewrites `<link rel="stylesheet">` URLs to content-hashed `.pagespeed.ce.` URLs that the module serves with a one-year `Cache-Control` max-age, so browsers keep stylesheets for a year and still pick up every change (a changed file gets a new URL). Use it when the origin cannot set long cache lifetimes itself; under CoreFilters it is already on through `extend_cache`. Disabling it leaves `extend_cache_images` and `extend_cache_scripts` on. The one rule to respect is the general one: HTML and the resources it references must share one configuration (see [Virtual hosts](/docs/configuration/#virtual-hosts)). Live demo: [extend_cache](/examples/extend_cache/).

Enable:

```apache
# Apache
ModPagespeedEnableFilters extend_cache_css
```

```nginx
# Nginx
pagespeed EnableFilters extend_cache_css;
```

### compute_critical_css {#compute_critical_css}

[Full guide →](/docs/filters/compute_critical_css/)

Not a core filter; experimental. Computes a page's critical CSS on the server in the background, instead of from the browser reports that [`prioritize_critical_css`](#prioritize_critical_css) uses. It is the module's older, beacon-free path and is not tuned for production: use `prioritize_critical_css` unless you are specifically testing this one. Enabling it adds server-side CSS analysis for every page it sees. There is no example in the gallery.

Enable:

```apache
# Apache
ModPagespeedEnableFilters compute_critical_css
```

```nginx
# Nginx
pagespeed EnableFilters compute_critical_css;
```

## Tuning parameters

| Parameter                | Default | Description                                                         |
| ------------------------ | ------- | ------------------------------------------------------------------- |
| `CssInlineMaxBytes`      | 2048    | Max CSS file size (bytes) to inline into HTML                       |
| `CssFlattenMaxBytes`     | 1024000 | Max size (bytes) of flattened CSS after resolving `@import`         |
| `CssOutlineMinBytes`     | 3000    | Min inline CSS size (bytes) to externalize                          |
| `CssImageInlineMaxBytes` | 0       | Max image size (bytes) to data-URI inline within CSS (0 = disabled) |

Apache syntax:

```apache
ModPagespeedCssInlineMaxBytes 4096
ModPagespeedCssFlattenMaxBytes 204800
ModPagespeedCssOutlineMinBytes 5000
ModPagespeedCssImageInlineMaxBytes 2048
```

Nginx syntax:

```nginx
pagespeed CssInlineMaxBytes 4096;
pagespeed CssFlattenMaxBytes 204800;
pagespeed CssOutlineMinBytes 5000;
pagespeed CssImageInlineMaxBytes 2048;
```

## See also

- [Filter selection](/docs/filter-selection/)
- [PageSpeed filters](/docs/filters/) — every filter in one table
- [JavaScript filters](/docs/javascript-filters/) — the matching minify, combine, and inline filters for JS
- [How CSS parsing works](/how-it-works/css-parsing/) — the syntax-tree layer beneath the CSS filters

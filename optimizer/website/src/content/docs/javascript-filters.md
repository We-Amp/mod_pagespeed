---
title: 'JavaScript filters'
description: 'Minify, combine, inline, and defer JavaScript in mod_pagespeed. Directives, defaults, and the trade-offs for each JS filter on Apache, nginx, and IIS.'
order: 45
group: 'Filters'
lastUpdated: 2026-11-13
---

## Overview

mod_pagespeed includes filters for JavaScript minification, combining, inlining, and deferring execution. Three JS filters are CoreFilters: `rewrite_javascript`, `combine_javascript`, and `inline_javascript`.

On pages mod_pagespeed recognizes as hydrated, some of these filters do not run. See [Hydrated pages](/docs/hydrated-pages/).

## Quick reference

| Filter                                                                    | Core | OFB | Description                                          | Safe          |
| ------------------------------------------------------------------------- | ---- | --- | ---------------------------------------------------- | ------------- |
| [`rewrite_javascript`](#rewrite_javascript)                               | Yes  | Yes | Minifies JS                                          | Yes           |
| `rewrite_javascript_external`                                             | Yes  | Yes | Implied by `rewrite_javascript`, external files only | Yes           |
| `rewrite_javascript_inline`                                               | Yes  | Yes | Implied by `rewrite_javascript`, inline scripts only | Yes           |
| [`combine_javascript`](#combine_javascript)                               | Yes  | No  | Combines multiple scripts                            | Yes           |
| [`inline_javascript`](#inline_javascript)                                 | Yes  | No  | Inlines small scripts                                | Yes           |
| [`defer_javascript`](#defer_javascript)                                   | No   | No  | Defers execution                                     | Test first    |
| [`outline_javascript`](#outline_javascript)                               | No   | No  | Externalizes large inline scripts                    | Experimental  |
| [`include_js_source_maps`](#include_js_source_maps)                       | No   | No  | Preserves source maps                                | Yes           |
| [`extend_cache_scripts`](#extend_cache_scripts)                           | Yes  | No  | Content-hashed script URLs with a one-year cache     | Yes           |
| [`canonicalize_javascript_libraries`](#canonicalize_javascript_libraries) | No   | No  | Swaps recognized libraries for a canonical URL       | Dangerous set |
| [`deterministic_js`](#deterministic_js)                                   | No   | No  | Deterministic Date and Math.random, for measuring    | Dangerous set |
| [`disable_javascript`](#disable_javascript)                               | No   | No  | Wraps scripts in noscript, for measuring             | Dangerous set |
| [`strip_scripts`](#strip_scripts)                                         | No   | No  | Removes all scripts, for measuring                   | Dangerous set |
| [`make_show_ads_async`](#make_show_ads_async)                             | No   | No  | Converts showads.js to async adsbygoogle.js          | Deprecated    |
| [`make_google_analytics_async`](#make_google_analytics_async)             | No   | No  | No-op: targeted the retired ga.js                    | Deprecated    |

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```text
pagespeed EnableFilters rewrite_javascript,combine_javascript
pagespeed JsInlineMaxBytes 2048
```

See [IIS configuration](/docs/iis-configuration/) for the full file format reference.

## rewrite_javascript {#rewrite_javascript}

<a id="rewrite_javascript_external"></a><a id="rewrite_javascript_inline"></a>

[Full guide →](/docs/filters/rewrite_javascript/) · Also: [`rewrite_javascript_external`](/docs/filters/rewrite_javascript_external/), [`rewrite_javascript_inline`](/docs/filters/rewrite_javascript_inline/)

### What it does

The three filters on this section share one minifier. It removes comments and collapses whitespace; identifiers, string literals, regular expressions and property names are emitted unchanged. It keeps a line break wherever automatic semicolon insertion could otherwise merge two statements into one. [How safe JavaScript minification handles automatic semicolon insertion](/blog/safe-javascript-minification-semicolon-insertion/) has the details.

```text
/* before: sum a shopping cart */
function cartTotal(cart) {
  var total = 0;  // running sum
  for (var i = 0; i < cart.items.length; i++) {
    total = total + cart.items[i].price;
  }
  return total;
}

/* after */
function cartTotal(cart){var total=0;for(var i=0;i<cart.items.length;i++){total=total+cart.items[i].price;}return total;}
```

`rewrite_javascript` is the compound CoreFilter for JavaScript minification: enabling it switches on the external and the inline sub-filter together, so every script on the page is minified wherever it lives. It is also part of OptimizeForBandwidth, the level that minifies resources in place without changing their URLs. To minify only one scope, enable `rewrite_javascript` and disable the other sub-filter by name. Under CoreFilters the compound is on already. A script delivered with an `integrity` attribute is left untouched by the whole family, because subresource integrity pins the file's bytes. Since v1.15.0+r21 the tokenizer-based minifier is the only one behind `rewrite_javascript`, and files that use template literals (backtick strings) minify normally; IE conditional-compilation comments (`/*@ ... @*/`) and a leading `#!` line are the only comments it keeps. Live demo: [rewrite_javascript](/examples/rewrite_javascript/).

`rewrite_javascript_external` handles the external half. Each `<script src>` file on a domain the module is authorized to fetch is minified and served from a rewritten `.pagespeed.jm.` URL with a long cache lifetime, so repeat visitors download the minified file once and keep it; the original file on disk is never modified. In OptimizeForBandwidth mode the minified bytes replace the original response in place and the URL stays as authored. Minification saves the most on hand-formatted source; a file a bundler already minified gains nothing and only costs rewrite time. `rewrite_javascript_external` runs as part of the compound and can also be enabled on its own.

Enabled on its own, `rewrite_javascript_external` leaves inline `<script>` blocks exactly as authored; minifying those is the inline sub-filter's job. Files on domains the configuration does not authorize keep their original URLs as well, so a page that mixes own and third-party scripts sees only the own files rewritten.

`rewrite_javascript_inline` minifies the contents of `<script>` blocks in the HTML itself, in place. No URL changes and nothing new is cached; the smaller script simply rides along inside every page view. Blocks whose `type` does not denote executable JavaScript, such as JSON-LD data islands and HTML templates, are left as they are. Inline scripts tend to be short, so the saving per block is small; the filter's job inside the compound is to leave no script unminified. Like the external half, `rewrite_javascript_inline` is in CoreFilters and in OptimizeForBandwidth, and it can be enabled by itself.

`rewrite_javascript_inline` never moves a block or changes when it runs; it only shrinks the text between `<script>` and `</script>`. On pages whose HTML is served many times between deploys that small saving repeats on every serve, which is where the filter earns its place.

### Risks

- The minifier never renames identifiers; the historical failure mode is a script that inspects its own source text, for example through `Function.prototype.toString()`, and reacts to the changed formatting.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-rewrite_javascript` comparison; [Is it working?](/docs/is-it-working/) has the steps.
- `rewrite_javascript` ignores the deprecated `UseExperimentalJsMinifier` directive, which is accepted for compatibility and logs a warning at configuration load (`ModPagespeedUseExperimentalJsMinifier` on Apache, `pagespeed UseExperimentalJsMinifier` on nginx). Remove it from your configuration.

### Configuration

**Apache:**

```apache
ModPagespeedEnableFilters rewrite_javascript
```

**Nginx:**

```nginx
pagespeed EnableFilters rewrite_javascript;
```

## combine_javascript {#combine_javascript}

[Full guide →](/docs/filters/combine_javascript/)

### What it does

`combine_javascript` concatenates consecutive external scripts into one file, loads the combined file from a `.pagespeed.jc.` URL, and replaces each original tag with a small inline script, `eval(…)`, that runs that member in order. A page that loads five scripts back to back makes one request instead of five. The scripts run in their original order, so dependencies between them keep working. Live demo: [combine_javascript](/examples/combine_javascript/).

```text
<!-- before -->
<script src="/js/jquery.js"></script>
<script src="/js/carousel.js"></script>
<script src="/js/forms.js"></script>

<!-- after -->
<script src="/js/jquery.js+carousel.js+forms.js.pagespeed.jc.HASH.js"></script><script>eval(mod_pagespeed_HASH1);</script>
<script>eval(mod_pagespeed_HASH2);</script>
<script>eval(mod_pagespeed_HASH3);</script>
```

### When it helps and when it does not

Combining was designed for HTTP/1.1, where a browser opens only a few connections per host and extra requests queue. Over HTTP/2 and HTTP/3 requests multiplex over one connection, so the saving shrinks to per-request overhead. The costs cut the other way too: the combined file is refetched in full when any member changes, and the browser cannot run the first script until the whole combined file has arrived. On a legacy page loading a dozen small files, combining still pays. On a page with two or three scripts over HTTP/2, measure with the filter on and off before keeping it.

### How it decides

Only consecutive, synchronously executing external scripts join a group. An inline `<script>`, a script with `async` or `defer`, a `type="module"` script, a script of an unknown type, and a script carrying `integrity=` each end the current group, as does other markup between two script tags. Modules are excluded because the combination evaluates member scripts inside one shared file, which cannot represent a module's isolated scope and deferred execution. A group stops growing when the combined uncompressed contents would pass `MaxCombinedJsBytes` (default 92160), and every member must come from a domain the module is authorized to fetch. Pages whose Content-Security-Policy forbids `eval` or inline scripts are not combined, because the browser would block the inline `eval` tags.

### Risks

- If a page misbehaves after combining, load it with `?PageSpeedFilters=-combine_javascript` and compare behavior and the console; the `X-Mod-Pagespeed` response header confirms whether the filter ran. [Is it working?](/docs/is-it-working/) covers the routine.

### Configuration

**Apache:**

```apache
ModPagespeedEnableFilters combine_javascript
ModPagespeedMaxCombinedJsBytes 92160
```

**Nginx:**

```nginx
pagespeed EnableFilters combine_javascript;
pagespeed MaxCombinedJsBytes 92160;
```

## inline_javascript {#inline_javascript}

[Full guide →](/docs/filters/inline_javascript/)

### What it does

`inline_javascript` replaces a small external script with an inline `<script>` block that holds the file's contents, so the browser skips a request. The element keeps its place in the page, so execution order does not change. Live demo: [inline_javascript](/examples/inline_javascript/).

```text
<!-- before: one extra request for a 1.4 KB file -->
<script src="/js/newsletter-popup.js"></script>

<!-- after: the file's contents sit inside the page -->
<script>document.addEventListener("DOMContentLoaded",function(){var d=document.getElementById("newsletter");d&&setTimeout(function(){d.hidden=!1},4e3)});</script>
```

### When it helps and when it does not

Inlining pays when a script is tiny: for a few hundred bytes, the request with its headers and round trip costs more than the bytes it fetches. The trade runs the other way as files grow or get shared. An inlined script is not cached on its own, so it downloads again with every page view, and a file inlined into ten pages is transferred ten times. Scripts that several pages share, and anything well above the default threshold, are better served external with a long cache lifetime. `inline_javascript` is a CoreFilter, so this trade is already live on a default install; the threshold is the knob.

### How it decides

Only external scripts whose contents are no larger than `JsInlineMaxBytes` (default 2048 bytes) qualify, and only files on domains the module is authorized to fetch. A script with `async` or `defer`, or with the IE-specific `for` and `event` attributes, is left external: those attributes change when the script runs, and an inline block cannot express that timing. Module scripts (`type="module"`) are never inlined, since inlining would change how their relative imports resolve. Nothing is inlined when the page's Content-Security-Policy forbids inline scripts. Everything else about the element, including its position in the document, stays as authored.

### Risks

- The page grows by the script's size on every view, so keep `JsInlineMaxBytes` small. Raising it to inline a large file usually costs more than the saved request returns.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-inline_javascript` comparison; [Is it working?](/docs/is-it-working/) has the steps.

### Configuration

**Apache:**

```apache
ModPagespeedEnableFilters inline_javascript
ModPagespeedJsInlineMaxBytes 2048
```

**Nginx:**

```nginx
pagespeed EnableFilters inline_javascript;
pagespeed JsInlineMaxBytes 2048;
```

## defer_javascript {#defer_javascript}

[Full guide →](/docs/filters/defer_javascript/)

Not a core filter. Test thoroughly before enabling. Defers execution of all JavaScript until after the page finishes loading. This can dramatically improve initial render time but will break scripts that rely on executing during page parse (e.g., `document.write`).

**Apache:**

```apache
ModPagespeedEnableFilters defer_javascript
```

**Nginx:**

```nginx
pagespeed EnableFilters defer_javascript;
```

### Risks

- Scripts using `document.write` will fail.
- Scripts that expect to run before `DOMContentLoaded` may break.
- Order-dependent scripts may execute in unexpected order.
- Inserts a `<noscript>` redirect by default. Disable with `SupportNoScriptEnabled false`.
- Since v1.15.0+r18, with `HonorCsp` (default on) the filter stands down entirely on pages whose `Content-Security-Policy` forbids inline scripts — deferral relies on injected inline JavaScript, which such a policy would block.
- Since v1.15.0+r21, `defer_javascript` — along with `disable_javascript`, `defer_iframe`, `fix_reflow`, and the shared `support_noscript` fallback — switches off for clients identified as automated, including clients that send no `User-Agent`; those clients receive the page's normal authored script markup. Search-engine crawlers are included deliberately. An automated client presenting a browser's exact user-agent string is indistinguishable from that browser and still receives the deferred form.

## outline_javascript {#outline_javascript}

[Full guide →](/docs/filters/outline_javascript/)

### What it does

`outline_javascript` is the inverse of `inline_javascript`: it moves a large inline `<script>` block out of the HTML into its own JavaScript file, served from a rewritten `_.pagespeed.jo.` URL with a long cache lifetime, and replaces the block with a `<script src>` pointing at it. The generated element is a copy of the original with the `src` added, so attributes such as `id` survive the move. Experimental and not a core filter; enable it by name. Live demo: [outline_javascript](/examples/outline_javascript/).

```html
<!-- before: 12 KB of script ride inside every page response -->
<script type="text/javascript" id="large">
  window.app = { ... };
</script>

<!-- after: the script is fetched once and kept in the browser cache -->
<script type="text/javascript" id="large" src="/_.pagespeed.jo.HASH.js"></script>
```

### When it helps and when it does not

A block of inline JavaScript is re-sent with every page view, and on pages whose HTML is generated per request those bytes cannot be cached at all. Outlining moves them into a file the browser fetches once and keeps, which is the same trade `outline_css` makes for stylesheets. It loses when the HTML itself is cached, when the block differs from page to page, or on pages visited once, because the first view pays an extra request it did not have before. Execution keeps its place in the document: an outlined script without `defer` or `async` still runs at the element's position, once the file has arrived. But `defer` and `async` on an inline script are ignored by browsers and take effect once the script is external, so a script carrying either runs later once outlined than it did inline. Most sites are better served by keeping script in real files or bundling them there at build time.

### How it decides

Only an inline script (one without `src`) classified as JavaScript is a candidate, and only when its text is at least `JsOutlineMinBytes` (default 3000 bytes). Two kinds are always left inline: a script carrying an `integrity` attribute, because the hash browsers ignore on an inline script would become enforced against the outlined bytes, and an inline module script, because outlining it would move import resolution from the document's base URL to the generated file's URL and change `import.meta.url`. A script the parser cannot hold as one unit, because a flush arrives mid-script or a stray tag sits inside it, is left alone rather than guessed at.

### Risks

- The outlined script is an extra request on the first view; on single-view pages that costs more than the HTML bytes it saves.
- Markup that expects the `<script>` element's contents to sit in the page, for example a script that reads its own source text, sees a `src` instead.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-outline_javascript` comparison; [Is it working?](/docs/is-it-working/) has the steps.

### Configuration

**Apache:**

```apache
ModPagespeedEnableFilters outline_javascript
ModPagespeedJsOutlineMinBytes 3000
```

**Nginx:**

```nginx
pagespeed EnableFilters outline_javascript;
pagespeed JsOutlineMinBytes 3000;
```

## include_js_source_maps {#include_js_source_maps}

[Full guide →](/docs/filters/include_js_source_maps/)

Not a core filter. Preserves JavaScript source maps through minification by adding a `//# sourceMappingURL=` comment pointing to the original source map. Enable this if you need to debug minified JavaScript in production.

**Apache:**

```apache
ModPagespeedEnableFilters include_js_source_maps
```

**Nginx:**

```nginx
pagespeed EnableFilters include_js_source_maps;
```

## extend_cache_scripts {#extend_cache_scripts}

[Full guide →](/docs/filters/extend_cache_scripts/)

Core filter, one of the three members of [`extend_cache`](/docs/cache-control/#extend_cache). Rewrites `<script src>` URLs to content-hashed `.pagespeed.ce.` URLs served with a one-year `Cache-Control` max-age: browsers keep scripts for a year, and a changed file gets a new URL, so there is nothing to purge. Use it when the origin cannot set long cache lifetimes itself; under CoreFilters it is already on through `extend_cache`. Disabling it leaves `extend_cache_css` and `extend_cache_images` on. Live demo: [extend_cache](/examples/extend_cache/).

**Apache:**

```apache
ModPagespeedEnableFilters extend_cache_scripts
```

**Nginx:**

```nginx
pagespeed EnableFilters extend_cache_scripts;
```

## JavaScript breaks after enabling the module {#javascript-breaks}

Find the filter first, then exclude the one script that needs it. The console error usually names the filter.

**1. Find the filter.** Request the broken page with the module switched off, then with one filter at a time switched off, and compare the browser console. Four filters touch scripts by default: `rewrite_javascript`, `combine_javascript`, `inline_javascript` and `extend_cache_scripts`. `defer_javascript` is not a CoreFilter, so it only runs if your configuration enabled it.

```text
https://example.com/page?PageSpeed=off
https://example.com/page?PageSpeedFilters=-combine_javascript
https://example.com/page?PageSpeedFilters=-defer_javascript
```

**2. Read the error.**

| Console error or symptom | Cause | What to do |
| --- | --- | --- |
| `ReferenceError: mod_pagespeed_… is not defined` | `combine_javascript` replaces each script with `eval(mod_pagespeed_…)`, and the combined file defines that variable. The error means the combined file did not arrive as JavaScript: it was a 404, an error or login page, or was blocked. | Open the combined `.pagespeed.jc.` URL from the page source in a new tab. A 404 is covered in [Why does a `.pagespeed.` URL return 404?](/docs/troubleshooting/#optimized-resources-return-404). With several servers behind a load balancer, check each one. |
| `pagespeed is not defined`, or `pagespeed.… is not a function` | The scripts that the module injects (the runtime behind `defer_javascript`, `lazyload_images` and the beacons) define the `pagespeed` object. The page ran code that uses it without that runtime, because it was not loaded or the HTML was cached from an older configuration. | The runtime is inlined or served from `/pagespeed_static/` (the [`StaticAssetPrefix`](/docs/configuration/#staticassetprefix)). Check that the path is not a 404 (on nginx, the `^/pagespeed_static/` location from the sample configuration), not blocked by a `Content-Security-Policy`, and purge cached copies of the HTML. |
| A script that calls `document.write` fails, or content appears in the wrong place | `defer_javascript` runs scripts after the page has parsed. It replays `document.write` calls, but only approximately. | Exclude the script (below), or leave `defer_javascript` off. |
| jQuery or another library starts too early or never | While deferred scripts are pending, the `defer_javascript` runtime makes `document.readyState` report `loading`, then `interactive`, and fires the load event afterwards. Code that reads `readyState` or listens for `DOMContentLoaded` at a different moment can run before the library it needs. | Exclude the library and the scripts that depend on it, or leave `defer_javascript` off. |
| Another script misbehaves, or the order of effects changes | One of the filters in step 1 changed how or when the script runs. | Find the filter with the URLs above, then exclude the script. |

**3. Exclude what breaks.** Pick the narrowest tool.

- **One `<script>` tag:** add the `data-pagespeed-no-defer` attribute (the older spelling `pagespeed_no_defer` also works). `combine_javascript` leaves such a script out of every group, and `defer_javascript` does not defer it. See [`data-pagespeed-no-defer`](/pagespeed-markers/#data-pagespeed-no-defer).

  ```html
  <script src="/js/payment.js" data-pagespeed-no-defer></script>
  ```

- **A file or a path:** [`Disallow`](/docs/configuration/#disallow) with a wildcard, for example `pagespeed Disallow "*/js/payment*";` (Apache: `ModPagespeedDisallow`). The module leaves matching resources alone.
- **A whole filter, for part of the site:** [`DisableFilters`](/docs/configuration/#disablefilters) in the `location` block (nginx) or the `<Location>` or `.htaccess` (Apache) that serves the affected pages.
- **A whole filter:** drop it from `EnableFilters`, or list it in `DisableFilters` at server level.

After a change, request the page once to let the module rebuild its copy, then reload. If you cannot tell which script is at fault, [`?PageSpeedFilters=+debug`](/docs/html-filters/) adds comments to the HTML that say what each filter did to each tag.

## Dangerous and deprecated filters

The module keeps these names so that an existing configuration still loads. The filters in the dangerous set are never switched on by `RewriteLevel AllFilters` and exist for measurement and testing, not for production traffic; the deprecated ones do nothing.

### canonicalize_javascript_libraries {#canonicalize_javascript_libraries}

[Full guide →](/docs/filters/canonicalize_javascript_libraries/)

Replaces a `<script src>` that matches a known library (recognized by size and hash through the `Library` directive) with the library's canonical URL on a shared CDN, so visitors reuse a copy already in their browser cache. In the dangerous set: the module ships no library table of its own any more, cross-site caches are partitioned in current browsers, and a canonical URL you do not control is a dependency you do not control. Use it only with your own `Library` entries and your own CDN. Live demo: [canonicalize_javascript_libraries](/examples/canonicalize_javascript_libraries/).

```nginx
pagespeed Library 105527 ltVVzzYxo0 //cdn.example.com/js/prototype.1.6.1.0.js;
pagespeed EnableFilters canonicalize_javascript_libraries;
```

### deterministic_js {#deterministic_js}

[Full guide →](/docs/filters/deterministic_js/)

Injects a script that makes `Date` and `Math.random` return deterministic values, so two loads of a page produce the same output and can be compared byte for byte. For measurement and regression testing only; it changes the behavior of every script on the page. In the dangerous set.

```nginx
pagespeed EnableFilters deterministic_js;
```

### disable_javascript {#disable_javascript}

[Full guide →](/docs/filters/disable_javascript/)

Wraps every `<script>` in `<noscript>` so no script on the page runs, to measure what the page looks like and costs without JavaScript. For measurement only. In the dangerous set.

```nginx
pagespeed EnableFilters disable_javascript;
```

### strip_scripts {#strip_scripts}

[Full guide →](/docs/filters/strip_scripts/)

Removes every `<script>` element from the page, the more drastic variant of `disable_javascript` for measuring the no-script baseline. For measurement only. In the dangerous set.

```nginx
pagespeed EnableFilters strip_scripts;
```

### make_show_ads_async {#make_show_ads_async}

[Full guide →](/docs/filters/make_show_ads_async/)

Rewrites synchronous `showads.js` ad snippets to the asynchronous `adsbygoogle.js` form so the ads stop blocking rendering. It targets a deprecated AdSense integration; convert the snippets in your templates instead. Live demo: [make_show_ads_async](/examples/make_show_ads_async/).

```nginx
pagespeed EnableFilters make_show_ads_async;
```

### make_google_analytics_async {#make_google_analytics_async}

[Full guide →](/docs/filters/make_google_analytics_async/)

Deprecated and a no-op: it rewrote the retired `ga.js` snippet to its asynchronous form. The name is accepted so old configurations load; remove it.

## Tuning parameters

| Parameter           | Default | Description                               |
| ------------------- | ------- | ----------------------------------------- |
| `JsInlineMaxBytes`  | 2048    | Max JS file size (bytes) to inline        |
| `JsOutlineMinBytes` | 3000    | Min inline JS size (bytes) to externalize |

**Apache:**

```apache
ModPagespeedJsInlineMaxBytes 2048
ModPagespeedJsOutlineMinBytes 3000
```

**Nginx:**

```nginx
pagespeed JsInlineMaxBytes 2048;
pagespeed JsOutlineMinBytes 3000;
```

## See also

- [Filter selection](/docs/filter-selection/)
- [PageSpeed filters](/docs/filters/) — every filter in one table
- [Safe JavaScript minification and semicolon insertion](/blog/safe-javascript-minification-semicolon-insertion/)
- [Remove unused JavaScript with Chrome coverage](/blog/remove-unused-javascript-chrome-coverage/)

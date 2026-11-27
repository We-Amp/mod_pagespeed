---
title: 'HTML filters'
description: 'HTML optimization filters in mod_pagespeed: collapse whitespace, strip comments, elide attributes, DNS prefetch, and resource preload hints.'
order: 46
group: 'Filters'
lastUpdated: 2026-10-22
---

## Overview

mod_pagespeed includes filters that optimize HTML structure, cut unnecessary bytes, and add performance hints. Two of them (`add_head`, `convert_meta_tags`) are CoreFilters and run by default; the rest are opt-in — see [Filter selection](/docs/filter-selection/) to turn them on. For resources rather than markup, see the [CSS filters](/docs/css-filters/), [JavaScript filters](/docs/javascript-filters/), and [Image filters](/docs/image-filters/) pages.

On pages mod_pagespeed recognizes as hydrated, some of these filters do not run. See [Hydrated pages](/docs/hydrated-pages/).

## Quick reference

| Filter                                                    | Core | Description                          | Safe               |
| --------------------------------------------------------- | ---- | ------------------------------------ | ------------------ |
| [`add_head`](#add_head)                                   | Yes  | Adds `<head>` if missing             | Yes                |
| [`convert_meta_tags`](#convert_meta_tags)                 | Yes  | Converts meta http-equiv to headers  | Yes                |
| [`collapse_whitespace`](#collapse_whitespace)             | No   | Removes excess whitespace            | Generally safe     |
| [`remove_comments`](#remove_comments)                     | No   | Strips HTML comments                 | Generally safe     |
| [`elide_attributes`](#elide_attributes)                   | No   | Removes default-value attributes     | Generally safe     |
| [`remove_quotes`](#remove_quotes)                         | No   | Removes unnecessary attribute quotes | Generally safe     |
| [`trim_urls`](#trim_urls)                                 | No   | Shortens absolute URLs to relative   | Generally safe     |
| [`combine_heads`](#combine_heads)                         | No   | Merges multiple `<head>` elements    | Generally safe     |
| [`pedantic`](#pedantic)                                   | No   | Adds type attributes for HTML4       | Generally safe     |
| [`insert_dns_prefetch`](#insert_dns_prefetch)             | No   | Adds DNS prefetch hints              | Generally safe     |
| [`hint_preload_subresources`](#hint_preload_subresources) | No   | Adds preload headers                 | Generally safe     |
| [`add_instrumentation`](#add_instrumentation)             | No   | Injects page load timing JS          | Test first         |
| [`add_base_tag`](#add_base_tag)                           | No   | Adds a `<base href>` for the page    | Test first         |
| [`add_ids`](#add_ids)                                     | No   | Adds ids to elements without one     | Test first         |
| [`insert_amp_link`](#insert_amp_link)                     | No   | Adds a `<link rel=amphtml>`          | Generally safe     |
| [`debug`](#debug)                                         | No   | Explains rewriting in comments       | Not for production |
| [`decode_rewritten_urls`](#decode_rewritten_urls)         | No   | Restores original resource URLs      | Test first         |
| [`compute_statistics`](#compute_statistics)               | No   | HTML statistics for the console      | Test first         |
| [`experiment_http2`](#experiment_http2)                   | No   | HTTP/2 features in development       | Experimental       |
| [`rule_packs`](#rule_packs)                               | No   | Applies a rule pack to the head      | Test first         |
| [`insert_ga`](#insert_ga)                                 | No   | Inserts the retired ga.js snippet    | Deprecated         |

## IIS syntax

On IIS, use the same filter names with the `pagespeed` prefix in `pagespeed.config` (no semicolons):

```text
pagespeed EnableFilters collapse_whitespace,remove_comments
```

See [IIS configuration](/docs/iis-configuration/) for the full file format reference.

## CoreFilters

### add_head {#add_head}

[Full guide →](/docs/filters/add_head/)

Adds a `<head>` element if the HTML lacks one. Several other filters inject content into `<head>`, so this filter ensures one exists. Runs automatically as a CoreFilter.

### convert_meta_tags {#convert_meta_tags}

[Full guide →](/docs/filters/convert_meta_tags/)

Reads `<meta http-equiv="Content-Type">` and similar tags and adds corresponding HTTP response headers. This helps browsers discover the content type and character encoding earlier in the response. Runs automatically as a CoreFilter.

## HTML minification filters

These filters reduce HTML payload size by removing unnecessary bytes.

### collapse_whitespace {#collapse_whitespace}

[Full guide →](/docs/filters/collapse_whitespace/)

#### What it does

`collapse_whitespace` shrinks the HTML payload by folding every run of whitespace (spaces, tabs, carriage returns, newlines) down to a single character. The markup keeps its structure; only the formatting bytes go. Live demo: [collapse_whitespace](/examples/collapse_whitespace/).

```text
<!-- before -->
<ul class="nav">
    <li>  <a href="/a">Alpha</a>  </li>
    <li>  <a href="/b">Beta</a>   </li>
</ul>

<!-- after: each run that held a newline keeps one newline -->
<ul class="nav">
<li> <a href="/a">Alpha</a> </li>
<li> <a href="/b">Beta</a> </li>
</ul>
```

#### When it helps and when it does not

The saving scales with how much pretty-printing the templates do: indented server-side templates carry many collapsible bytes, already-compacted HTML almost none. Gzip and Brotli compress whitespace runs well, so the on-the-wire saving is much smaller than the source saving suggests. The filter earns most where HTML is served uncompressed, and least on an already minified template. Not a CoreFilter; enable it by name.

#### How it decides

A whitespace run that contains a newline collapses to a newline, any other run to a single space; a run never collapses to nothing, so inline elements keep their separating space and text layout is preserved. Content inside `pre`, `code`, `script`, `style`, and `textarea` is never touched, because whitespace is meaningful there.

#### Risks

- Pages whose rendering depends on whitespace-sensitive CSS, such as `white-space: pre` on ordinary elements, can change appearance; the filter sees the markup, not the stylesheet. Test such pages before enabling.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-collapse_whitespace` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters collapse_whitespace
```

```nginx
# Nginx
pagespeed EnableFilters collapse_whitespace;
```

### remove_comments {#remove_comments}

[Full guide →](/docs/filters/remove_comments/)

#### What it does

`remove_comments` deletes HTML comments (`<!-- ... -->`) from the served page, cutting bytes that only developers read. The document tree is unchanged apart from the removed nodes. Live demo: [remove_comments](/examples/remove_comments/).

```html
<!-- before -->
<!-- TODO: replace with the component version -->
<div class="banner">Sale ends Friday</div>

<!-- after -->
<div class="banner">Sale ends Friday</div>
```

#### When it helps and when it does not

It helps on pages whose templates carry heavy commentary: build markers, TODO notes, and section labels add up on large documents. It does nothing for pages that ship few comments, and it must not remove comments that carry a function. Comments you are obliged or willing to ship stay behind a `RetainComment` wildcard, and IE conditional comments (`<!--[if IE]> ... <![endif]-->`) are parsed as directives rather than comments, so they always survive.

#### How it decides

Every comment node is dropped unless its text matches one of the configured `RetainComment` wildcard patterns. There is no size threshold and no content analysis beyond the pattern match.

#### Risks

- Copyright or license notices that must ship with the page need a `RetainComment` entry before the filter goes on.
- A rare third-party snippet that reads the page's own comments breaks; retain its marker comment or disable the filter for that path. Verify with the `X-Mod-Pagespeed` header and `?PageSpeedFilters=-remove_comments`; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters remove_comments
ModPagespeedRetainComment "*copyright*"
```

```nginx
# Nginx
pagespeed EnableFilters remove_comments;
pagespeed RetainComment "*copyright*";
```

### elide_attributes {#elide_attributes}

[Full guide →](/docs/filters/elide_attributes/)

Removes HTML attributes that are set to their default values. For example, `<form method="get">` becomes `<form>` because `get` is the default method.

### remove_quotes {#remove_quotes}

[Full guide →](/docs/filters/remove_quotes/)

Removes unnecessary quotation marks around HTML attribute values when the value contains no special characters. Saves a few bytes per attribute.

### trim_urls {#trim_urls}

<a id="left_trim_urls"></a>

[Full guide →](/docs/filters/trim_urls/)

#### What it does

`trim_urls` shortens URLs inside the page by stripping the parts that repeat the page's own origin. An absolute URL whose scheme, host and port match the page loses its origin, and the page's own directory prefix is trimmed too; URLs on any other origin, including the same host over another scheme, are left untouched. `left_trim_urls` is an accepted alternate spelling of the same filter; both names switch on the same code. Live demo: [trim_urls](/examples/trim_urls/).

```html
<!-- page: https://example.com/shop/ -->
<!-- before -->
<a href="https://example.com/shop/cart">Cart</a>
<img src="https://example.com/img/logo.png" />

<!-- after -->
<a href="cart">Cart</a>
<img src="/img/logo.png" />
```

#### When it helps and when it does not

It saves a few bytes per URL on pages dense with same-origin absolute links, which is typical of CMS output that expands every URL in full; under gzip or Brotli the saving shrinks further, since the repeated origin strings compress well. It does not help pages that already use relative URLs throughout. Keep it off for HTML that lives beyond its origin: a saved page, an emailed copy, or markup served under a second domain resolves relative URLs against the wrong base.

#### How it decides

Each URL-valued attribute is resolved against the page's base URL, and a `<base>` tag wins when present and is never itself rewritten. Only what matches gets trimmed: a full origin match drops the origin, a path under the page's own directory then drops that directory as well, and a URL on another origin keeps its full form. A trim is kept only when the shorter URL resolves back to exactly the original one.

#### Risks

- Serving the same cached HTML from multiple domains, or any flow that detaches the markup from its origin, turns the trimming into broken links; disable `trim_urls` for that content.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-trim_urls` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters trim_urls
```

```nginx
# Nginx
pagespeed EnableFilters trim_urls;
```

## Structural filters

### add_base_tag {#add_base_tag}

[Full guide →](/docs/filters/add_base_tag/)

Adds a `<base href="…">` element with the page's own URL to `<head>`, so relative URLs in the page resolve against the URL the module rewrote them for. Use it when the HTML is served at a URL other than the one it was authored for (proxy setups, `MapProxyDomain`). Risk: a page that already relies on a different base, or on the absence of one, resolves its relative links differently; test navigation and form actions.

```nginx
pagespeed EnableFilters add_base_tag;
```

### add_ids {#add_ids}

[Full guide →](/docs/filters/add_ids/)

Adds an `id` attribute to elements that have none, so that beacon-driven filters can refer to individual elements across page loads. Rarely needed on its own: the filters that need ids enable it themselves. Risk: scripts or styles that count on the exact set of ids in the page see extra ones.

```nginx
pagespeed EnableFilters add_ids;
```

### combine_heads {#combine_heads}

[Full guide →](/docs/filters/combine_heads/)

#### What it does

`combine_heads` folds extra `<head>` elements into the first one. When the module's HTML parse closes a second or later `<head>`, the contents of that element move into the first head and the emptied element disappears, so the served document carries exactly one head with everything in it. Not a core filter; enable it by name. Live demo: [combine_heads](/examples/combine_heads/).

```html
<!-- before: two fragments, each contributing its own head -->
<head>
  <title>Report</title>
</head>
...the first fragment's content...
<head>
  <link rel="stylesheet" href="/css/part2.css" />
</head>
...the second fragment's content...

<!-- after: one head, both fragments' head content in it -->
<head>
  <title>Report</title>
  <link rel="stylesheet" href="/css/part2.css" />
</head>
...the first fragment's content... ...the second fragment's content...
```

#### When it helps and when it does not

It helps on pages assembled from fragments that each ship a complete document skeleton: server-side includes that embed whole HTML files, portal or search pages that splice in results blocks with their own `<head>`, scraped or migrated content pasted with its original head. A browser's own parser discards the stray `<head>` tags and keeps the content, so such pages mostly render by accident; the merged output is predictable and gives the module's head-injecting filters one place to work. On a normal page with a single head the filter changes nothing.

#### How it decides

The first `<head>` in the document is the survivor. Each later `</head>` moves its children into that first head, in document order. The merge cannot cross a flush boundary: when the server streams the page in chunks, heads that close in a later chunk than the first head stay as they are, because content already flushed to the browser cannot be restructured. The filter shares its implementation with [`add_head`](#add_head): the same pass that inserts a missing `<head>` performs the merge, so enabling `combine_heads` also brings `add_head`'s behavior of inserting a head into documents that have none.

#### Risks

- Content ordering inside the merged head follows the fragments, not any intent the fragments had; a fragment whose styles or scripts assumed their original position relative to other elements should be checked after enabling.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-combine_heads` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters combine_heads
```

```nginx
# Nginx
pagespeed EnableFilters combine_heads;
```

### pedantic {#pedantic}

[Full guide →](/docs/filters/pedantic/)

Adds `type="text/javascript"` and `type="text/css"` attributes to `<script>` and `<style>` elements. This satisfies HTML4 validators. Not needed for HTML5, where these types are the defaults.

## Performance hint filters

### insert_dns_prefetch {#insert_dns_prefetch}

[Full guide →](/docs/filters/insert_dns_prefetch/)

#### What it does

`insert_dns_prefetch` adds connection warm-up hints for the origins of resources referenced in the body that the head does not already reference. A `<link rel="dns-prefetch">` hint starts the DNS lookup early, while the browser is still busy with the HTML; a `<link rel="preconnect">` hint goes further and opens the connection, TCP and for HTTPS also TLS, before the resource tag is even seen. Live demo: [insert_dns_prefetch](/examples/insert_dns_prefetch/).

```html
<!-- inserted into <head> -->
<link rel="preconnect" href="https://fonts.examplecdn.com" />
<link rel="dns-prefetch" href="//analytics.example.com" />
```

#### When it helps and when it does not

It helps when a page pulls from a few stable third-party origins, such as font CDNs or analytics hosts: the lookup and handshake then overlap with the HTML download instead of starting when the resource is discovered. It does nothing for same-origin resources, since the connection to the page's own origin is already open. It also does little when the set of third-party domains churns between page views, because the hints are learned from earlier rewrites of the page and may name domains it no longer uses.

#### How it decides

The filter records which origins a page's body resources come from, in order of first appearance. Hints are emitted once the number of hinted domains changes by at most two between rewrites; the check counts domains, so a page whose domains change but whose count holds still gets hints for the stored list. A page gets at most eight hints in total; the first two domains get `preconnect`, the rest `dns-prefetch`. Early views contribute data and get nothing; later views get the hints. Domains the author already hinted or referenced in the head are not duplicated.

#### Risks

- Every hint costs the browser work, and a preconnect costs an open connection held for an origin the visitor might not need; the built-in caps of eight hints, at most two of them preconnects, bound that overhead.
- Verify with the `X-Mod-Pagespeed` response header and a `?PageSpeedFilters=-insert_dns_prefetch` comparison; [Is it working?](/docs/is-it-working/) has the steps.

#### Configuration

```apache
# Apache
ModPagespeedEnableFilters insert_dns_prefetch
```

```nginx
# Nginx
pagespeed EnableFilters insert_dns_prefetch;
```

### hint_preload_subresources {#hint_preload_subresources}

[Full guide →](/docs/filters/hint_preload_subresources/)

Adds `Link: rel=preload` HTTP headers for CSS and JavaScript files discovered on previous visits to the same page. Uses the beacon system to collect resource data, so it becomes effective after the first page view.

Since v1.15.0+r21, `<script type="module">` subresources are hinted with `rel=modulepreload` in the `Link` response header instead of `rel=preload`; modules carrying `integrity` or `crossorigin="use-credentials"` are left unhinted.

### insert_speculation_rules {#insert_speculation_rules}

[Full guide →](/docs/filters/insert_speculation_rules/)

Injects a same-origin prefetch `<script type="speculationrules">` block so that supporting browsers prefetch a link as the visitor starts interacting with it; browsers without speculation-rules support ignore the tag. Only same-origin links are eligible.

The filter stands down in several cases rather than injecting a ruleset that would be wasted or unsafe: it backs off when the page already carries its own speculation ruleset, when a `Content-Security-Policy` forbids inline scripts, on non-200 responses, on cookie-setting responses, on `no-store` responses, and on AMP documents.

The injected ruleset is fixed: one rule that prefetches same-origin document URLs (`href_matches: "/*"`) with `eagerness: moderate`, meaning the browser applies its own heuristic for when a prefetch is worth starting rather than prefetching every link on sight. There is no prerender and nothing cross-origin. The script is placed at the end of `<body>`. Requests that negotiate markdown (`Accept: text/markdown`), such as AI-agent fetches, are also left clean: they run no scripts and get no benefit, so the tag stays out of the variant those caches key on under `Vary: Accept`.

Speculative prefetch spends origin bandwidth on navigations that may never happen, so treat it as a trade-off and test it against your own traffic. This filter is opt-in and is not part of any rewrite level. Enable it by name:

**Apache:**

```apache
ModPagespeedEnableFilters insert_speculation_rules
```

**nginx:**

```nginx
pagespeed EnableFilters insert_speculation_rules;
```

### insert_amp_link {#insert_amp_link}

[Full guide →](/docs/filters/insert_amp_link/)

Adds a `<link rel="amphtml">` to `<head>` pointing at the page's AMP version, built from the `AmpLinkPattern` directive. Use it when you publish AMP pages at a predictable URL pattern and want every canonical page to announce its AMP twin. Risk: a pattern that produces URLs that do not exist advertises broken AMP pages to crawlers.

```nginx
pagespeed AmpLinkPattern "https://amp.example.com${url}";
pagespeed EnableFilters insert_amp_link;
```

## Debugging and measurement filters

### debug {#debug}

[Full guide →](/docs/filters/debug/)

Annotates the page with HTML comments that say which filters ran and why a resource was or was not rewritten (for example why an image was not inlined or a stylesheet not combined). Enable it per request with `?PageSpeedFilters=+debug` while troubleshooting instead of in the configuration: it exposes internals, enlarges every page, and is not meant for production traffic.

```text
https://www.example.com/?PageSpeedFilters=+debug
```

### decode_rewritten_urls {#decode_rewritten_urls}

[Full guide →](/docs/filters/decode_rewritten_urls/)

Turns `.pagespeed.` resource URLs in the page back into the original resource URLs, undoing the URL rewriting of the other filters. Useful in a proxy chain or when debugging what the page referenced before optimization. Risk: the page then references unoptimized resources, which defeats the filters that depend on rewritten URLs.

```nginx
pagespeed EnableFilters decode_rewritten_urls;
```

### compute_statistics {#compute_statistics}

[Full guide →](/docs/filters/compute_statistics/)

Computes statistics about the HTML (element counts and sizes) for the [admin console](/docs/admin-console/). It adds a parsing pass per page and changes nothing in the output; enable it while you need the numbers.

```nginx
pagespeed EnableFilters compute_statistics;
```

### experiment_http2 {#experiment_http2}

[Full guide →](/docs/filters/experiment_http2/)

Switches on HTTP/2-specific behavior that is still in development. Experimental: what it does can change between releases, and it is not covered by the compatibility promises of the other filters.

```nginx
pagespeed EnableFilters experiment_http2;
```

## Analytics filters

### insert_ga {#insert_ga}

[Full guide →](/docs/filters/insert_ga/)

Inserts the Google Analytics snippet for the account in `AnalyticsID` into every page. Deprecated: the snippet it inserts is the retired `ga.js`, and `AnalyticsID` itself only targets Universal Analytics, which was discontinued. Add your analytics in your templates instead.

## Rule packs

### rule_packs {#rule_packs}

[Full guide →](/docs/filters/rule_packs/)

Applies the rules of a rule pack, a JSON file named by `RulePackFile`, to the canonical link, title, meta description, hreflang cluster and JSON-LD of your pages. It is off by default and in no rewrite level: it is in the dangerous set, so `RewriteLevel AllFilters` does not enable it either. A pack starts in report mode (`RulePackMode report`, the default) and records what it would change without changing anything. See [Rule packs](/docs/rule-packs/) for the pack format, enforce mode and signed packs.

```nginx
pagespeed EnableFilters rule_packs;
pagespeed RulePackFile /etc/pagespeed/packs/example.com.json;
```

## Deprecated and dangerous filters

The names below are still accepted so that an existing configuration keeps loading, with a warning. The deprecated ones do nothing at all; `fix_reflows` and `mobilize` are in the dangerous set, which `RewriteLevel AllFilters` never enables, and exist for experiments rather than production. Remove them from your configuration.

### cache_partial_html {#cache_partial_html}

[Full guide →](/docs/filters/cache_partial_html/)

Deprecated no-op.

### defer_iframe {#defer_iframe}

[Full guide →](/docs/filters/defer_iframe/)

Deprecated no-op: iframe deferral is built into [`defer_javascript`](/docs/javascript-filters/#defer_javascript); enabling this name alone never did anything.

### div_structure {#div_structure}

[Full guide →](/docs/filters/div_structure/)

Deprecated no-op.

### explicit_close_tags {#explicit_close_tags}

[Full guide →](/docs/filters/explicit_close_tags/)

Deprecated no-op.

### flush_subresources {#flush_subresources}

[Full guide →](/docs/filters/flush_subresources/)

Deprecated no-op.

### fix_reflows {#fix_reflows}

[Full guide →](/docs/filters/fix_reflows/)

Experimental fix for layout reflows caused by deferred JavaScript. In the dangerous set; not for production.

### mobilize {#mobilize}

[Full guide →](/docs/filters/mobilize/)

The retired page-mobilization experiment. In the dangerous set; not for production.

### mobilize_precompute {#mobilize_precompute}

[Full guide →](/docs/filters/mobilize_precompute/)

Deprecated no-op.

### split_html {#split_html}

[Full guide →](/docs/filters/split_html/)

Deprecated no-op.

### split_html_helper {#split_html_helper}

[Full guide →](/docs/filters/split_html_helper/)

Deprecated no-op.

## add_instrumentation {#add_instrumentation}

[Full guide →](/docs/filters/add_instrumentation/)

Injects JavaScript that measures page load time and reports it back to the mod_pagespeed statistics system via [the beacon endpoint](/pagespeed-markers/#pagespeed-beacon) (`/mod_pagespeed_beacon` or `/ngx_pagespeed_beacon`). Enable this filter to get client-side performance data in the admin console histograms.

Test this filter before deploying to production. The injected JavaScript adds a small overhead and sends beacon requests on every page load.

Since v1.15.0+r18, with `HonorCsp` (default on) nothing is injected on pages whose `Content-Security-Policy` disallows the instrumentation script — no beacon fires for those pages, so they contribute no data to the console histograms.

## See also

- [PageSpeed filters](/docs/filters/) — every filter in one table
- [Filter selection](/docs/filter-selection/)

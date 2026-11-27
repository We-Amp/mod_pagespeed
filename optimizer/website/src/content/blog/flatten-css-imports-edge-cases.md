---
title: "Flattening CSS @imports: harder than concat"
description: "Why flattening CSS @import rules is harder than concatenation: media intersection, @charset aborts, url() rebasing, and the CssFlattenMaxBytes cap."
date: 2026-06-13
lastUpdated: 2026-10-22
tags: ["css", "performance", "deep-dive", "mod-pagespeed-1.1", "filters"]
product: "1.1"
draft: false
---
## Why `@import` is slow

A CSS `@import` rule hides a request the browser cannot see in advance. The browser does not learn that `base.css` exists until it has fetched and parsed the parent sheet that imports it. If `base.css` then imports `colors.css`, that third request waits on the second. Each link in the chain is a serialized round trip, discovered late, with no chance for the browser to fetch the files in parallel the way it does with side-by-side `<link>` tags. On a high-latency connection, a three-deep import chain can add hundreds of milliseconds before any styling arrives. Flattening removes the chain outright; where a sheet cannot be flattened, [server-injected resource hints](/blog/server-injected-resource-hints-speculation-rules/) are the fallback that at least lets the browser discover the nested files without parsing their parents first.

The fix sounds trivial: replace each `@import` with the bytes of the file it points at, recurse, and emit one stylesheet. The mod_pagespeed module does exactly this with the `flatten_css_imports` filter, a CoreFilter that runs by default. The reason it took a design document and not an afternoon is that a correct flattener has to preserve the meaning of the original cascade, and several CSS rules make naive concatenation produce a stylesheet that behaves differently from the original.

This post walks the non-obvious cases a flattener has to get right. It complements the one-line entry in the [CSS filters reference](/docs/css-filters/#flatten_css_imports); here we explain why the rules exist.

## Media queries: intersection, not wrapping

An `@import` can carry a media type: `@import url(print.css) print;`. The imported rules apply only to that medium. The obvious move is to wrap the imported file in `@media print { ... }`. That works until the imported file has its own `@media` blocks, because CSS does not allow `@media` rules to nest.

So flattening cannot wrap. It has to push the media condition down onto each rule and combine it with whatever media that rule already carries. The combination is an **intersection**, not a concatenation. Consider importing a sheet with `print`, where the sheet itself contains a `@media screen` block. A rule that is screen-only inside a sheet that is print-only can never match anything: `screen` and `print` do not overlap. The flattener drops that rule rather than emitting a contradictory `@media screen and print` wrapper. Rules with no media of their own simply inherit the import's media. Rules whose media overlaps the import keep the intersection.

Get this wrong and you ship rules that apply on the wrong medium, or drop rules that should have stayed. It is the single most error-prone part of the operation, and the reason "just concatenate the files" is not a correct flattener.

## `@charset`: abort on a real conflict

A CSS file may declare its encoding with `@charset`, and the spec requires it to be the very first bytes of the file. When you splice an imported file into the middle of another, a stray `@charset` would land somewhere it is not allowed, and worse, it might declare an encoding different from the parent document's.

mod_pagespeed handles this conservatively. If an imported file's `@charset` matches the encoding already in effect, the rule is redundant after flattening and gets removed. If it declares a **different** encoding, the byte interpretation of that file genuinely differs from the rest, and flattening cannot safely merge them. The filter aborts and leaves the original `@import` in place. A correct-but-unoptimized stylesheet beats a single sheet that misreads half its own bytes.

## Relative URLs have to be rebased

A stylesheet at `/css/theme/base.css` that references `url(../img/bg.png)` resolves that path relative to its own location. Inline its rules into `/css/app.css` and the relative path now resolves against a different directory. The background image breaks.

Flattening therefore rewrites every `url()` in an imported file to an absolute URL computed against the importing file's location, recursively, as it descends the chain. The base URL changes at each level of the import tree, so the filter tracks a stack of base URLs: push when it enters an imported file, pop when it leaves. This is the same class of problem that any CSS-combining or rewriting step has to solve, and it is why correct CSS optimization needs a real parser rather than search-and-replace. A regex that rewrites `url(...)` without understanding comments will happily mangle a URL that only appears inside `/* ... */`. The same parser-not-regex discipline governs the JavaScript side, where [safe minification hinges on automatic semicolon insertion](/blog/safe-javascript-minification-semicolon-insertion/) rather than pattern matching. mod_pagespeed parses the CSS first, which is also what lets the same pipeline content-hash and cache-extend the assets it finds. (See [how content-hashed URLs work](/blog/content-hash-urls/) for that side of it.)

## 404 and 5xx are not the same failure

When an imported file cannot be fetched, the response status tells you how to react.

A **404** means the file is gone. The browser would have skipped that import and rendered the rest. So the flattener skips the missing import and keeps going, matching what the page would have done anyway.

A **5xx** (a 502 or 503, say) means the file probably exists but the origin had a transient problem. Inlining the rest and silently dropping rules that might come back on the next request would change the page's styling in a way that flickers as the origin recovers. So on a server error the filter aborts the whole flattening and leaves the imports untouched. The page still works using the browser's own `@import` fetching; it just is not flattened this time.

The same caution applies to parse errors. If any file in the chain will not parse cleanly, flattening stops, because `@import` rules legally sit only at the top of a stylesheet and there is no safe place to leave a half-flattened result.

## The size cap: `CssFlattenMaxBytes`

Flattening trades requests for bytes. One request is better than five, but not if the single sheet balloons to something enormous that delays first paint more than the round trips it removed. `CssFlattenMaxBytes` (default `1024000`, one megabyte) caps the flattened result. If the combined CSS would exceed it, the filter declines to flatten. Lower it if your import graph pulls in vendor frameworks you would rather keep as separately cacheable files. Both the parameter and the per-engine syntax are listed in the [CSS filters tuning table](/docs/css-filters/#flatten_css_imports), and `flatten_css_imports` is summarized in the [filters reference](/docs/filters/).

## In the module versus the optimizer worker {#in-115-versus-20}

In the module, `flatten_css_imports` is an in-process CoreFilter that runs on the response, with the rules above. The optimizer worker flattens imports too, but inside its separate optimization [worker process](/blog/migrating-from-1x/) rather than in the web-server request path. There it resolves the import chain during CSS processing and caches the result as a single content-hashed resource, toggled with `--no-css-import-flattening`. The two parts share the same goal — collapse the serialized chain into one resource — but the worker-based pipeline does it as part of its CSS path, and we do not claim its handling of every edge case above is byte-for-byte identical to the module filter.

The original mod_pagespeed authors also floated ideas that never shipped, such as caching each flattened import separately for reuse across sheets. Treat that as a design note from the era, not a feature.

## Try it

`flatten_css_imports` is on by default in the module, so if you are running it, your import chains are already being collapsed. To see the rest of what the CSS pipeline does — minification, critical-CSS extraction, cache extension — start from the [feature list](/features/), or read [how the optimizer parses CSS](/how-it-works/css-parsing/) and how [the optimizer worker extracts critical CSS](/blog/critical-css-heuristics/).

---

*The design rules in this post descend from the original mod_pagespeed @import-inlining design (the Apache PageSpeed project — We-Amp was an initial committer on that project alongside the original Google engineers), summarized here as background. The behavior described is the current reference for the module.*

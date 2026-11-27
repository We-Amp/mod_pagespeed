---
title: 'How CSS minification and URL rewriting work'
description: "How mod_pagespeed parses CSS into a syntax tree to minify, rewrite url(), and flatten @import safely — and why regex rewriters corrupt stylesheets."
order: 30
datePublished: 2026-06-13
lastUpdated: 2026-11-15
---
Most "CSS minifiers" are a pile of regular expressions. Strip whitespace, delete comments, collapse `0px` to `0`. That works until it doesn't, and when it fails it fails silently: the page still loads, the stylesheet still parses in the browser, but one background image now points at the wrong path. mod_pagespeed treats CSS the way a browser does. It parses the stylesheet into a syntax tree before touching a byte. This page explains why that matters, and what a real parser makes possible that string-munging cannot.

## The corruption case that motivates a parser

Here is a stylesheet that breaks naive rewriters. The `url()` token is split across a comment:

```css
#a { color: green }  /* url(../img/old
#b { color: blue }      .png) */
#c { background: url(../img/hero.png) }
```

A search-and-replace pass that scans for `url(` and rewrites whatever follows has no idea the first match is inside a `/* */` comment. It sees `url(../img/old`, never finds the closing paren on the same line, and either rewrites garbage or mangles the comment boundary. The real `url()` on the third line — the one that actually needs rewriting because the file is moving directories — may get skipped or doubled.

This is not a contrived example. It is close to the exact case the original mod_pagespeed authors used to justify building a parser in 2010, when their early CSS combining still relied on search-and-replace. The lesson generalizes: any tool that reasons about CSS as a flat string instead of as nested structure will eventually corrupt input that is legal CSS but doesn't match the regex author's mental model. Comments, strings, escaped characters, nested functions, and `data:` URIs all hide tokens that look like other tokens.

## What a syntax tree actually buys you

Once the stylesheet is a tree of rules, selectors, declarations, and values rather than a string, several optimizations become correct by construction instead of correct by luck.

**Whitespace and comment removal that stays safe.** The minifier removes whitespace it can prove is insignificant and drops comments, because it knows which bytes are inside a string literal or a `url()` and which are structural. It collapses redundant syntax and trims trailing decimal zeros. It is editing a tree and serializing it back, not deleting characters that match a pattern.

**`url()` absolutification when files move.** Relative URLs in CSS resolve against the location of the stylesheet, not the HTML page. The moment the optimizer combines two stylesheets into one resource at a new path, or inlines CSS into the document, every relative `url()` has to be rewritten so it still resolves to the same asset. The parser knows exactly which value tokens are URLs — including the ones hiding in comments that should be left alone — so it rewrites the real references and ignores the decoys.

**`@import` expansion in order.** A stylesheet that begins with `@import "base.css";` pulls in another file before its own rules apply, and import order is load-bearing for the cascade. Flattening those imports into one resource (the [`flatten_css_imports` filter](/blog/flatten-css-imports-edge-cases/)) requires expanding them in the correct sequence. You cannot get cascade order right by concatenating strings; you need to understand where each `@import` sits in the rule list and what it pulls in.

**Embedded image rewriting.** Background images referenced from CSS are images too. With a parsed tree, the optimizer can route a `background: url(logo.png)` through the same image pipeline that handles `<img>` tags — recompression, format conversion, and content-hashed URLs — because it can find the URL token reliably and replace it with the optimized variant. Those rewritten URLs carry a content hash so they can be cached for a year without ever serving stale bytes; see [why optimized URLs carry a content hash](/blog/content-hash-urls/).

## The safety net: `fallback_rewrite_css_urls`

No parser handles every stylesheet ever written. Vendor hacks, malformed input, and CSS features newer than the parser all exist in the wild. So the optimizer never bets everything on the full parse succeeding.

When the parser cannot make sense of a stylesheet, the `fallback_rewrite_css_urls` filter takes over and does the one transformation that is still safe without full structure: it rewrites the resource URLs inside the CSS and leaves the rest of the bytes untouched. You give up minification and combining for that file, but the embedded images and `@import` targets still get correct, cache-friendly URLs. The page keeps working. This filter is on by default in the CoreFilters set precisely because it is the graceful-degradation path — full structural rewrite when the parse works, URL-only rewrite when it doesn't, and the original bytes if even that isn't safe. The behavior is documented in the [CSS filters reference](/docs/css-filters/).

## Ideas the original authors considered but did not build

The 2010 design note floated a wishlist that went well beyond minification: strip CSS rules that no element on the page uses, rename classes to shorter names, simplify over-complicated selectors, and refactor rules to remove redundancy. These are real ideas, and they are seductive. They are also not what mod_pagespeed shipped then, and not what it ships now. The reason is in the design note itself: JavaScript can change an element's classes at runtime, so a rule that looks unused at page load may be needed the instant a user clicks something. Dead-code elimination and class renaming on live HTML are unsafe in the general case, so they stayed on the drawing board.

What the parser actually powers in production is the conservative set: minify, rewrite embedded URLs, combine adjacent stylesheets, and flatten `@import`. Every CSS pass also only keeps its output when the result is genuinely smaller than the input. We mention the aspirational rewrites here so the distinction is clear — the parser could see the structure needed for them, but seeing the structure and being safe to act on it are different problems.

## The module and the optimizer worker: same parsing discipline {#115-and-20-same-parsing-discipline}

Both parts fully parse CSS for structural rewrites and fall back to URL-only rewriting on parse failure. The runtime differs. The module runs in-process inside Apache, nginx, or IIS and continues the original `CssParser` lineage. The optimizer worker runs the same parsing in a [separate process](/how-it-works/async-rewriting/) behind a thin reverse-proxy layer, and adds SVG output to the image pipeline that CSS background images feed into. The parsing logic — tree first, fall back to URL-only, keep the smaller result — is the constant across both. The same discipline underlies [server-side critical CSS extraction](/blog/server-side-critical-css-nginx/), which has to understand selectors to decide what is above the fold — the same selector awareness behind the [critical-CSS extraction heuristics](/blog/critical-css-heuristics/).

A structure-aware server optimizer is categorically safer than the string substitution most build-time hacks rely on. It edits CSS without corrupting the stylesheet on the input you didn't think to test.

The CSS parser at the center of this design comes from the original mod_pagespeed project: Joshua Marantz wrote the 2010 design note that argued for a real parser over search-and-replace, and that lineage carries straight into the current mod_pagespeed, in both of its parts. mod_pagespeed is an open-source project now maintained by We-Amp B.V.

You can run the full optimizer — licensed under Apache-2.0, free to run — and watch it rewrite a stylesheet on your own pages; see [what mod_pagespeed optimizes](/features/) to start.

---
title: "mod_pagespeed and Content-Security-Policy (CSP)"
description: 'Does mod_pagespeed break a strict Content-Security-Policy? Where rewriting collides with a CSP, feature by feature, and how HonorCsp keeps pages compliant.'
date: 2026-06-13
lastUpdated: 2026-10-22
tags: ["security", "csp", "configuration", "mod_pagespeed"]
product: "1.1"
draft: false
---
If you run a strict Content-Security-Policy and you want an automatic optimizer in front of your pages, you are right to be cautious. The two systems are working against each other. A CSP exists to constrain exactly what a page is allowed to load and execute. An optimizer earns its keep by changing what a page loads and executes. Put them together without thought and the optimizer rewrites a resource to a URL the policy never authorized, the browser refuses it, and a page that worked five minutes ago stops rendering.

This post walks through where mod_pagespeed and a CSP actually collide, feature by feature, and how the mod_pagespeed module handles it: with the [`HonorCsp`](/docs/configuration/#native-module-configuration-directives) directive, which makes the optimizer read your policy and decline any rewrite it cannot prove the policy already allows. The short version is that the safe behavior is to refuse the rewrite, not to widen your policy to fit it.

## Why automatic rewriting and CSP fight

A CSP's job is to pin down sources. `script-src 'self' cdn.example.com` says scripts may come from your own origin and that one CDN, and nowhere else. `img-src 'self'` says images come from your origin only, and not from `data:` URIs. `style-src 'self'` blocks inline `<style>` unless you opt back in with `'unsafe-inline'`, a nonce, or a content hash. It is defense in depth against injection: if an attacker manages to inject a `<script>` tag, a tight `script-src` stops it from pulling in arbitrary remote code.

mod_pagespeed changes the same things the policy is trying to lock down. It moves resources to a CDN. It rewrites an asset's URL to carry a [content hash](/blog/content-hash-urls/). It combines several files into one. It inlines a small image as a `data:` URI or a small stylesheet straight into the HTML. Every one of those is a source change, and a source change is precisely what a CSP is built to reject.

There is a second, subtler reason the two are hard to reconcile, and the original design work on this called it out plainly. You cannot assume the HTML reaching the optimizer is trustworthy. The injection hole a CSP defends against might sit upstream of mod_pagespeed, in the application that generated the page. So an optimizer must not *legalize* anything the policy would have blocked. If a URL would have been refused as authored, rewriting it into an allowed form would quietly undo the protection. The conservative rule that falls out of this: only rewrite when the result still satisfies the policy you were handed.

## The conflict classes, one at a time

### Content-hashed URLs versus exact-URL source lists

This is the central collision. When mod_pagespeed optimizes a resource it gives the result a new name that encodes the filter and a hash of the output bytes, like `logo.png.pagespeed.ic.7Vx0aLd9Wf.png`. That hash is what lets the asset be cached for a year without ever going stale, and it is also unpredictable by design: change the input or the settings and the URL changes.

Now suppose your policy listed the resource by its exact path. The rewritten URL is not on that list, so the browser blocks it. You cannot pre-authorize the optimized URL either, because the hash is a function of output that may not exist yet. mod_pagespeed runs optimizations in the background and serves cached results to live traffic, so the rewritten name for any given request is not knowable in advance.

The original authors sketched three responses: loosen the policy to directory granularity, try to predict the optimized URL and set the header to match, or block the rewrite when the existing policy is too narrow to permit it. The module takes the third. If your policy is directory-level and the rewritten URL falls inside an allowed path, the rewrite proceeds. If your policy named an exact file, mod_pagespeed declines to rewrite it rather than guess at a URL or widen what you authorized. To be clear about what the module does *not* do: it does not attempt user-agent-dependent URL prediction, and it does not try to reconcile a policy that differs from page to page. Those were explored on paper; the shipped behavior is the conservative subset.

### CDN and library mapping that expands allowed sources

When you ask mod_pagespeed to move resources to a CDN with [`MapRewriteDomain`](/docs/cache-control/#rewrite_domains), the rewritten HTML points at the new host. A policy that allowed `www.example.com` but not `cdn.example.com` will block those resources. This case is gentler than the hash case, because you explicitly asked for the CDN, so extending the allowance to the mapped host is at least defensible. Library canonicalization (swapping a local jQuery for a copy on a shared CDN) is the same shape of change. Both still amount to teaching the browser about a source it was told to distrust, so they belong under the same rule: act only within what the policy permits.

### Combining across paths

[`combine_css`](/docs/css-filters/#combine_css) and `combine_javascript` merge several files into one. The combined file has to live somewhere, and that somewhere is typically a common ancestor directory. If your policy allowed `/a/b/` and `/a/c/` but not `/a/`, the merged file lands at a path the policy never approved. Widening the policy to `/a/` to accommodate the merge would loosen your security to suit an optimization, which is backwards. The safe move is to skip the combine when it would produce a URL the policy does not already cover.

JavaScript combining carries an extra wrinkle the original design flagged: the technique it described relied on `eval`, which a policy without `'unsafe-eval'` forbids. The authors' own conclusion was that this is not something an optimizer should switch on for you under a strict policy. Treat aggressive JS combining as opt-in, and verify it against your `script-src` before enabling it in production. Automatic JavaScript rewriting has [correctness edge cases of its own](/blog/safe-javascript-minification-semicolon-insertion/) worth understanding before you widen a policy to accommodate it.

### Inlining images as data: URIs

`inline_images` replaces a small image reference with a `data:` URI, removing an HTTP request. That only works if your policy includes `data:` in `img-src`. With a plain `img-src 'self'`, the inlined image is blocked. The two options are to add `data:` to your `img-src` yourself (a deliberate policy change you make, not one the optimizer makes for you) or to leave the image as a normal request. With `HonorCsp` on, mod_pagespeed takes the second path and declines to inline rather than emit a `data:` URI your policy rejects.

### Inline script and style, nonces, and hash invalidation

CSP gives two ways to allow specific inline content: a nonce that matches the policy, or a content hash of the inline block listed in the policy. Both interact badly with rewriting if handled carelessly.

The nonce path has a trap. If a policy uses `'unsafe-inline'` and you add a nonce to make a newly injected inline block legal, you can break the page, because the presence of a nonce causes browsers to ignore `'unsafe-inline'` entirely. The first nonce you add silently disables the blanket allowance the rest of the page was relying on. So an optimizer must not start sprinkling nonces into a policy that was leaning on `'unsafe-inline'`.

The hash path has a different trap. If an inline `<script>` or `<style>` is already authorized by a hash in the policy and the optimizer changes its contents, even just to update a URL inside it, the old hash no longer matches and the browser blocks the block. Recomputing the hash to match is fiddly: the browser and the optimizer's HTML parser do not necessarily fold whitespace identically, so the recomputed hash can disagree on bytes that look the same to a human. The dependable behavior is to leave hash-pinned inline content alone unless the change is safe to account for. The module with `HonorCsp` avoids modifying inline content where doing so would invalidate a policy that authorized it by hash.

### base-uri and page semantics

`base-uri` restrictions can make a `<base>` tag inoperable, which changes how relative URLs resolve. The original notes raised the unsettling case where a browser that does not enforce `base-uri` resolves URLs differently from one that does, so correctness becomes user-agent dependent. mod_pagespeed reads `base-uri` as part of honoring the policy rather than assuming a `<base>` tag is always in effect.

## What HonorCsp does, and what it doesn't

[`HonorCsp`](/docs/configuration/#native-module-configuration-directives) is on by default in the module. The optimizer parses the `Content-Security-Policy` headers on a response and uses them as a gate: a rewrite happens only when its output still satisfies the policy. When in doubt, it declines the rewrite and serves the resource unchanged. You lose a little optimization on the affected resources; you keep a working, policy-compliant page. The directive is written like this (set it to `off` to disable the check):

```nginx
# nginx
pagespeed HonorCsp on;
```

```apache
# Apache
ModPagespeedHonorCsp on
```

```text
# IIS (pagespeed.config)
pagespeed HonorCsp on
```

Two limits. First, `HonorCsp` reads the policy from HTTP response headers. The cleanest setup is to deliver your CSP site-wide on headers rather than only in `<meta>` tags, so the optimizer has the policy available when it needs it. Second, `HonorCsp` is a conservatism switch, not a policy generator. It will not author or relax a CSP for you, and it makes no attempt to handle a policy that varies from page to page or to predict a rewritten URL per user agent. Those were design explorations the original project considered, not behavior that shipped. If you want maximum optimization under a strict policy, the productive direction is to write your CSP at directory granularity where you can, and to include `data:` in `img-src` deliberately if you want inlining, rather than expecting the optimizer to widen the policy on your behalf.

A note on the optimizer worker: the `HonorCsp` directive described here is the module's. The optimizer worker has a different architecture, with optimization in a separate [worker process](/how-it-works/async-rewriting/), and its CSP handling is not a drop-in match for the directive. If you are on a strict policy today, validate against the module behavior documented above and test the worker against your own policy before relying on it.

## Where this came from

CSP support in mod_pagespeed grew out of design work on the [Apache PageSpeed project](https://github.com/we-amp/mod_pagespeed/wiki), specifically a 2016 design note by Maksim Orlovich that mapped out exactly these conflict classes before any of them were implemented. We-Amp was an initial committer on that project alongside the Google engineers who started PageSpeed, as the project moved toward Apache incubation. The conservative stance this post describes (refuse the rewrite rather than legalize it) is the design's own conclusion, and it is what We-Amp ships in the [maintained mod_pagespeed](/mod-pagespeed-still-maintained/) today.

If you want to try it — mod_pagespeed is licensed under Apache-2.0 and free to run — install it, then put a strict policy in front of a page and watch how `HonorCsp` behaves against it. The full directive set is in the [configuration reference](/docs/configuration/), and the feature overview is on the [features page](/features/).

---

*This article draws on the original Apache PageSpeed design documentation (2010–2018), in particular Maksim Orlovich's 2016 note on PageSpeed and Content-Security-Policy. Original material released under the Apache License 2.0. mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with Google and maintains mod_pagespeed independently.*

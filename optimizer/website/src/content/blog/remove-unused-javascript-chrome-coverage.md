---
title: 'Remove unused JavaScript with Chrome coverage data'
description: 'A headless Chrome V8 coverage pass measures which JavaScript runs before first paint, so the mod_pagespeed worker can defer the scripts that never run.'
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ["performance","javascript","headless-chrome","core-web-vitals","deep-dive"]
draft: false
product: '2.0'
---

Lighthouse tells you a page ships 380 KB of JavaScript and that "212 KB was unused." Useful, but it stops there. It does not tell you which of those bytes were never touched during the part of the load that matters, and it certainly does not rewrite the page for you. To **remove unused JavaScript** safely you need two things Lighthouse does not give you: per-script execution data measured at first paint, and a classifier that knows when moving a script is safe. The optimizer worker gets the first by running the page through Chrome's V8 coverage instrumentation in a headless tab, then feeds the result into a small set of deferral rules. This post is about exactly what that pass measures, which CDP methods it calls, and where the signal stops being trustworthy.

## What the coverage pass actually measures

The analyzer lives in `ScriptCoverageAnalyzer` (`src/browser/script_coverage_analyzer.cc`). It drives Chrome over the DevTools Protocol (CDP) and uses V8's precise coverage, not the Debugger domain. Once the tab is attached and the sandbox is set up (network forced offline, `Fetch.enable`, viewport set), the instrumentation is turned on with two calls before any content is injected:

```
Profiler.enable
Profiler.startPreciseCoverage  { callCount: false, detailed: true }
```

`detailed: true` is what makes this useful. It asks V8 for coverage at the level of byte ranges inside each function, not just a per-function executed/not-executed flag. `callCount: false` means the analyzer does not care how many times a range ran, only whether it ran at all.

The page is then rendered in a network-blocked sandbox. The HTML is injected with `Page.setDocumentContent` against the frame from `Page.getFrameTree`, the network is forced offline with `Network.emulateNetworkConditions { offline: true }`, and every subresource request is intercepted with `Fetch.enable` and answered from the worker's in-memory cache via `Fetch.fulfillRequest` (or `Fetch.failRequest` with `BlockedByClient` when the URL is not cached). JavaScript stays enabled here, because the whole point is to let scripts run so V8 can record what executes.

Coverage is sampled twice, driven by lifecycle events from `Page.setLifecycleEventsEnabled`:

- On `firstContentfulPaint`, the analyzer calls `Profiler.takePreciseCoverage` and stores the result as the FCP snapshot.
- On `networkIdle`, it calls `Profiler.takePreciseCoverage` again for the load snapshot.

`takePreciseCoverage` returns delta-only data: each call resets V8's counters, so the second snapshot contains only what executed *after* FCP. The code is explicit about this in `MergeCoverageMaps`, which sums the two disjoint snapshots to reconstruct cumulative coverage at load. Finally `Profiler.stopPreciseCoverage` is sent and the tab is closed with `Target.closeTarget`.

Each script in the coverage payload arrives as a list of functions, each with `ranges` carrying `startOffset`, `endOffset`, and `count`. Function ranges nest (an outer function range contains its inner block ranges), so a naive byte sum double-counts. `ComputeCoverageByScript` collects every range with `count > 0`, then `MergeRangesAndComputeUsed` sorts and merges overlaps before summing used bytes. `total_bytes` per script is taken as the maximum `endOffset` seen. The output per script is two numbers: `coverage_at_fcp` and `coverage_at_load`, each a fraction of bytes executed.

This is the JavaScript counterpart to the CSS path. The CSS extractor runs with `Emulation.setScriptExecutionDisabled` and uses the `CSS.startRuleUsageTracking` / `CSS.takeCoverageDelta` family instead of the Profiler domain, because used-CSS is about which rules matched, not which code ran. The sandbox and cache-feeding mechanics are shared, and getting that feed right matters a lot. We wrote up the failure mode separately in [Feeding Chrome's Coverage API](/blog/css-cache-inlining-for-coverage-api/) — read that for the CSS mechanics; this post does not repeat them.

## Deciding which unused JavaScript is safe to remove

A coverage fraction on its own is not an instruction. A script can sit at 0% coverage at FCP and still be unsafe to move, because moving JavaScript can change ordering and break pages. The classifier in `ScriptCoverageAnalyzer::Classify` turns the measured `ScriptInfo` into one of four `DeferralAdvice` values, in this order:

1. **`kAlreadyAsync`** — the script already has `async`, `defer`, or `type="module"`. Nothing to do.
2. **`kKeepSynchronous`** — the script calls `document.write`, or modifies the DOM before paint. Leave it alone.
3. **`kSafeToDefer`** — `coverage_at_fcp` is exactly `0.0`, with no `document.write`. None of its bytes ran before first paint, so deferring it cannot change what the user first sees.
4. **`kCandidateForAsync`** — `coverage_at_fcp` is below `0.10` (under 10% of bytes executed at FCP) with no `document.write`. A little ran, but most of it is dead weight on the critical path; a candidate for `async` rather than a clean defer.

Anything else (meaningful execution at FCP) falls through to `kKeepSynchronous`. The `document.write` check is deliberately conservative: the DOM-collection script flags a script when its inline text matches `/document\.write/`, and any positive match pins the script synchronous regardless of coverage. `document.write` after the parser has moved on can blow away the document, so a 0% reading does not buy you anything there.

That conservatism is the whole design. The coverage number tells you a script is *unused* on the critical path; the attribute and `document.write` checks tell you whether acting on that is *safe*. Only scripts that clear both gates get a defer or async recommendation. The script element metadata that feeds those gates (`is_async`, `is_defer`, `is_module`, `has_document_write`, plus a CSS selector for each `<script>`) is gathered by a `Runtime.evaluate` call against the live DOM, using a `JSON.stringify` reference captured before page scripts can override it.

One field in `ScriptInfo` is worth calling out: `modifies_dom_before_paint` exists in the struct and is checked by the classifier, but the header marks it "Not yet implemented (always false)." So today the synchronous-pin decision rests on `document.write` detection plus the FCP coverage threshold. The hook for richer pre-paint DOM analysis is wired; the detection behind it is not yet.

## Where the signal stops

Precise coverage measures execution during one headless load. That boundary is the most important thing to understand before you trust a recommendation.

The pass watches a single render up to `networkIdle`. It does not observe scrolling, clicks, route changes, or anything a real session does after the page settles. A script whose entire job is to wire up a "buy" button will read as 0% coverage at load, because nobody clicked anything in a headless tab. `kSafeToDefer` is the right call for such a script (deferring an interaction handler past first paint is exactly what you want), but "0% coverage at load" must be read as "did not run during initial load," never as "is dead code you can delete." The analyzer recommends deferral, not deletion, and that distinction is deliberate.

A few other limits fall out of how the measurement works:

- **It is keyed by URL.** `ComputeCoverageByScript` keys each script by its `url`, falling back to V8's `scriptId` only when the URL is empty. Inline scripts are labelled `inline-N` by ordinal during DOM collection, which will not line up with V8's keying — so coverage attribution is reliable for external scripts, weaker for inline blocks.
- **The merge assumes disjoint deltas.** Summing FCP and post-FCP used bytes is correct precisely because `takePreciseCoverage` resets counters between snapshots. The code clamps the sum to `total_bytes` to guard the edge cases, but the model is delta-based, not a re-scan.
- **One render, one viewport.** `Analyze` takes a single `viewport_width`/`viewport_height` and runs to a default 60-second session timeout (`kDefaultTimeoutMs`). A script gated behind a media query that only fires on mobile will read differently depending on which viewport you measured. This is the same execution-on-load caveat the [INP-focused work](/core-web-vitals/inp/) cares about: the JavaScript that hurts interaction latency often runs *after* load, which is exactly the window this pass cannot see.

None of this makes the signal weak. It makes it specific. The coverage pass answers one question precisely — "which script bytes executed before the page finished loading?" — and the classifier acts only where acting is safe. That is a far better basis for deferring JavaScript than a static parse, and a far safer one than deleting code because a tool called it "unused."

INP is the Core Web Vital most directly tied to main-thread JavaScript, and deferring code that never runs on load is one of the cleaner levers for it — most visibly on [WordPress, where plugin scripts pile onto the critical path](/blog/fix-inp-wordpress-2026/).

## Related

- [Feeding Chrome's Coverage API: Inlining Cached CSS](/blog/css-cache-inlining-for-coverage-api/) — the CSS sibling of this pass, and the cache-feeding bug that returns 0% if you get it wrong.
- [Critical CSS Heuristics](/blog/critical-css-heuristics/) — how the same coverage signal drives critical CSS decisions.
- [Safe JavaScript Minification and Semicolon Insertion](/blog/safe-javascript-minification-semicolon-insertion/) — the other JavaScript transform that has to prove it is safe before it acts.
- [Fixing INP on WordPress](/blog/fix-inp-wordpress-2026/) — the interaction-latency side of unused JavaScript.
- [Fix INP on nginx: a CMS-agnostic plan](/blog/fix-inp-nginx-2026/) — the server-layer view of deferring main-thread JavaScript.
- [Can AI Read Your Website?](/blog/can-ai-read-your-website/) — what headless rendering reveals about a page beyond performance.

The optimizer worker runs this coverage pass as part of its headless analysis stage; the recommendations feed the same async-rewriting machinery described in [how async rewriting works](/how-it-works/async-rewriting/). If you want to see what it flags on your own pages, [download a build](/download/) and read the [browser-analysis docs](/docs/browser-analysis/) for how to enable the headless stage. It is licensed under Apache-2.0 and free to run, so you can measure on your own pages.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

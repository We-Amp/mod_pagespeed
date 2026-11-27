---
title: 'Server-injected resource hints: Speculation Rules and more'
description: "How a proxy generates server-injected resource hints: speculation-rules prefetch derived from real traffic, priority-bucketed preconnect, font preload."
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ["performance","core-web-vitals","nginx","deep-dive"]
draft: false
product: "2.0"
---

The browser already knows how to prefetch the next page, preconnect to a font CDN, and preload an LCP image. The problem is that someone has to write those `<link>` and `<script>` tags into the HTML, keep them in sync with what the page actually references, and not break anything when a third-party origin moves. Most sites never do it.

Server-injected resource hints push that work to the optimizing layer. On the cache-miss path — [the same server-side pass that shapes TTFB](/blog/reduce-ttfb-server-layer-2026/) — the mod_pagespeed optimizer worker parses the HTML once to collect facts, then writes a second pass that injects three kinds of hint: a `<script type="speculationrules">` block whose prefetch list is derived from real traffic, `<link rel="preconnect">` for cross-origin hosts ranked by how render-critical they are, and `<link rel="preload" as="font">` for woff2 faces. This post walks through how each one is built, with the constants and exclusions taken straight from the worker source so you know exactly what ships and what is off by default.

## Speculation rules from a real-traffic hot-URL tracker

The interesting hint is prefetch. A static prefetch list goes stale the moment your navigation patterns change. The optimizer worker instead derives the speculation URL list from traffic it actually sees.

Every warmup request feeds a hot-URL tracker. The relevant code in `worker.cc` is small:

```cpp
if (cfg->enable_speculation_rules && !notification.hostname.empty()) {
  RecordHotUrl(notification.scheme + "://" + notification.hostname +
               notification.url);
}
```

`RecordHotUrl` maintains an LRU: a `std::deque<std::string>` (front = newest) plus an `unordered_set` for O(1) dedup. A URL already present is moved to the front; when the deque reaches `kMaxHotUrls` (50) the oldest entry is evicted. So the tracker holds the 50 most-recently-seen URLs per worker, weighted toward recency.

When a page is being assembled, the transform path asks for the prefetch candidates:

```cpp
if (cfg->enable_speculation_rules) {
  speculation_urls = GetHotUrls(notification.hostname, 10);
  has_speculation = !speculation_urls.empty();
}
```

`GetHotUrls` returns at most 10 URLs and applies two filters that matter for correctness and safety. First, same-hostname only: it parses `scheme://host` out of each stored URL and skips anything whose host does not match the page being served, so you never prefetch across sites. Second, a hardcoded exclusion list:

```cpp
static constexpr std::string_view kExcludePatterns[] = {
    "/api/", "/logout", "/cart", "/checkout", "/admin", "/wp-admin",
};
```

Any URL containing one of these substrings is dropped. Prefetching `/logout` or `/checkout` is exactly the kind of helpful-but-wrong behavior that turns a speedup into a support ticket, so those paths never make it into a speculation block.

The list then goes to `InjectSpeculationRules` in `html_transform_filter.cc`, which builds the JSON by hand with escaping rather than string-concatenating attacker-influenced URLs:

```cpp
std::string json = R"({"prefetch":[{"source":"list","urls":[)";
for (const auto& url : speculation_urls_) {
  if (!IsAllowedPreloadUrl(url)) continue;
  if (!first) json += ',';
  first = false;
  json += '"';
  json += net_instaweb::JsonEscapeHtmlSafe(url);
  json += '"';
}
```

Every URL is run through `IsAllowedPreloadUrl`, which only accepts `https://`, `http://`, or path-absolute (`/`, but not protocol-relative `//`) URLs, and each is passed through `JsonEscapeHtmlSafe` before it lands inside a `<script>`. If every candidate is filtered out the block is not emitted at all. The result is a single `<script type="speculationrules">` inserted before `</body>`, tagged with `data-pagespeed-hint` so the next revalidation pass can strip and regenerate it cleanly.

One caveat: speculation rules are **opt-in**. The worker config defaults `enable_speculation_rules = false`, and it is turned on with the `--enable-speculation-rules` flag. The hot-URL tracker only records traffic when that flag is set, so there is no cost when you leave it off. Speculative prefetch consumes origin bandwidth and CPU on behalf of visitors who may never click, which is a deliberate trade-off rather than a free win, so it stays behind a flag.

## Preconnect, bucketed by render priority

Preconnect is cheaper and on by default (`disable_preconnect_injection` is `false`; you turn it off with `--no-preconnect-injection`). The hard part is choosing *which* origins, because the browser caps how many simultaneous connection warm-ups help before they start competing for bandwidth.

The scan pass in `html_scanner.cc` collects every cross-origin host referenced by the page and tags each with a priority bucket:

```cpp
constexpr int kPriorityRenderBlocking = 0;  // head stylesheets + sync head scripts
constexpr int kPriorityHeadResource  = 1;   // other head resources
constexpr int kPriorityBodyResource  = 2;   // images + any script in the body
```

Bucketing is by what blocks rendering, and it depends on where the element sits. Render-blocking (bucket 0) is a `<link rel="stylesheet">` or a synchronous `<script src>` in the head, because the connection setup is on the render-blocking path. Head resources (bucket 1) are other head links and any `async` or `defer` script in the head. Body resources (bucket 2) are images plus any `<script src>` in the body, including a synchronous one. So `async`/`defer` does not by itself mean bucket 2: a deferred script in the head is still bucket 1, and what pushes a script to bucket 2 is being in the body. Each origin is recorded once (a `seen_origins_` set prevents duplicates), and only if it differs from the page's own origin.

At end of document the origins are ranked and trimmed:

```cpp
std::stable_sort(origin_entries_.begin(), origin_entries_.end(),
                 [](const OriginEntry& a, const OriginEntry& b) {
                   return a.priority < b.priority;
                 });
size_t count = std::min(origin_entries_.size(), size_t{4});
```

A `stable_sort` keeps document order within a bucket, then the top 4 survive. So the four origins you preconnect to are the four most render-critical cross-origin hosts on the page, render-blocking first. `InjectPreconnectLinks` writes each as `<link rel="preconnect" crossorigin data-pagespeed-hint>`, again gated on `IsAllowedPreloadUrl`. If you have one font CDN and one analytics host, both get warmed; if you have a dozen third parties, the page does not drown in connection setups for hosts that only matter below the fold. These links ship inside the HTML body; the same preload data can also be delivered ahead of the document, as a [103 Early Hints response keyed off a sentinel cache entry](/blog/sentinel-cache-keys-and-103-early-hints/).

## Font preload and the woff2 extractor

The third hint is `<link rel="preload" as="font">`. `font_url_extractor.cc` walks `@font-face` blocks in the page's CSS and pulls the best URL out of each `src:` value, with a strong preference for woff2:

```cpp
bool is_woff2 = format_woff2 || LooksLikeWoff2(url);
if (is_woff2) {
  return url;  // Found a woff2 URL — return immediately.
}
```

A source qualifies as woff2 either by an explicit `format('woff2')` hint after the `url(...)` or by a `.woff2` extension (with query strings and fragments stripped before the extension check). `data:` URLs are skipped, the extracted set is deduplicated, and extraction caps at `kMaxFontUrls` (10). The injector, `InjectFontPreload`, emits each surviving URL as `<link rel="preload" as="font" type="font/woff2" crossorigin data-pagespeed-hint>`.

Be precise about the status here, because the code can mislead at a glance. Font preload exists and is tested, but it is **present in the engine and not wired into the shipped assembly path**: there is no flag or config key that turns it on today. `enable_font_preload` defaults to `false` in `HtmlTransformConfig` and nothing in the worker sets it true, and the worker constructs `HtmlTransformFilter` without passing a `font_urls` list, so the default deployment does not inject font preloads. Treat it as a building block that exists in the engine, not as a behavior you get for free today. The speculation and preconnect paths above are the ones the worker actively drives.

## Idempotency: regenerating server-injected resource hints

All three injectors share a property that matters more than it looks. The worker re-optimizes pages it has already optimized, so every injected element carries a marker — `data-pagespeed-hint` on links and scripts, `data-pagespeed-critical` on injected style. On the next pass `StartElement` deletes any element carrying that marker before re-injecting:

```cpp
if ((keyword == HtmlName::kLink || keyword == HtmlName::kScript) &&
    (element->FindAttribute("data-pagespeed-hint") != nullptr)) {
  parser_->DeleteNode(element);
  modified_ = true;
  return;
}
```

That is what keeps a page from accumulating five stale speculation blocks and a dozen duplicate preconnects over its cache lifetime. The hints regenerate from current facts each time the page is assembled, which is the whole point of deriving them from live traffic in the first place. For how that assembly fits the broader cache-miss flow, see [/how-it-works/async-rewriting/](/how-it-works/async-rewriting/).

## Related

- [/blog/sentinel-cache-keys-and-103-early-hints/](/blog/sentinel-cache-keys-and-103-early-hints/) — the other side of hint delivery: a preload list stored at a sentinel cache key and emitted as a 103 Early Hints response, not in the HTML body.
- [/blog/proactive-variant-generation-cache-warmup/](/blog/proactive-variant-generation-cache-warmup/) — image-variant warmup, the warmup mechanism that also feeds the hot-URL tracker.
- [/blog/headless-lcp-cls-measurement/](/blog/headless-lcp-cls-measurement/) — how the LCP candidate and browser profile that drive other hints are measured.
- [/blog/reduce-ttfb-server-layer-2026/](/blog/reduce-ttfb-server-layer-2026/) — where server-injected hints sit in the request path relative to TTFB.
- [/blog/fix-lcp-wordpress-2026/](/blog/fix-lcp-wordpress-2026/) — applying preload and preconnect to a WordPress front end.
- [/core-web-vitals/lcp/](/core-web-vitals/lcp/) — what these hints move on LCP, and what they do not.

If you want to see the speculation block on your own traffic, run the worker with `--enable-speculation-rules`, let it observe a few page loads to populate the hot-URL tracker, and inspect the bottom of an assembled page for the `<script type="speculationrules">` block. The [download](/download/) is licensed under Apache-2.0 and free to run, so you can validate the behavior on your own traffic. The [configuration reference](/docs/configuration/) lists every `--no-*` and `--enable-*` toggle that controls hint injection.

---
*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*

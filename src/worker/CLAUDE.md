# Factory Worker

Lightweight C++ daemon handling optimization notifications from nginx.

## IPC Protocol

Non-blocking fire-and-forget. Nginx creates a Unix socket per
notification (O_NONBLOCK + poll 50ms), sends message, closes. Default: no
retries (prevents blocking nginx event loop). Optional retry with exponential
backoff (configurable via `RetryConfig`). Timeouts not retried.

**Wire format**: `[4B length][4B url_len][url][4B host_len][hostname][1B content_type][4B mask]`

If all retries exhausted, cache serves original. Subsequent requests re-notify
(natural retry via fallback-hit path).

## Shared Config File

The worker writes `pagespeed-shared.conf` on startup and on `PATCH /v1/config`
changes. Location: `parent_path(cache_path) / "pagespeed-shared.conf"`.

**Format:** `key=value` (one per line)

**Fields:**
- `socket_path` — Unix socket path for nginx notifications
- `html_mode` — `off`, `markdown-static`, `markdown-rendered` or `full`
  (optional key, no schema bump; absent = `full`). A valid value wins over
  `disable_html` whatever the line order; an unknown value is ignored
- `disable_html` — legacy form, written as `true` exactly when `html_mode` is
  `off`, so a reader that predates `html_mode` keeps working (it sees every
  `markdown-*` mode as `full`). Read only when `html_mode` is absent or
  unknown: `true` = `off`, `false` = `full`

Since 2.1 there is no license state anywhere in the daemon: no
`license_key` / `license_valid` / `license_checked_once` in this file, no
`pagespeed.license` or `pagespeed.instance-id` sidecar, no `/v1/license/*`
routes, no `license` object in `/v1/health`, no `x-pagespeed-warn` header. The
`agent_optimize_entitled` key is still written but simply mirrors the operator
`--agent-optimize` flag. `--license-key` / `--license-renewal-url` (and the
`PAGESPEED_LICENSE_*` env vars / `pagespeed.json` keys) are accepted, ignored,
and answered with a one-time deprecation warning so 2.0 configs still start.

Nginx reads this file automatically (polled every ~1s) using `pagespeed_cache_path`
to locate the parent directory. This replaces the former `pagespeed_worker_socket`
and `pagespeed_critical_css` nginx directives.

## Production Hardening

- Max connections: 128 (`--max-connections`)
- Buffer cap: 1MB (`--max-buffer-size`)
- Connection timeout: 30s (`--connection-timeout`)
- Graceful shutdown: drains with 5s timeout (`--shutdown-timeout`)
- Health check: `{socket_path}.health` → `OK {active}/{max} notifs=N variants=N ...`

## HTTP Management API

Embedded HTTP/1.1 server (libuv + llhttp) for the web console and programmatic access.

**Files**: `http_server.h/.cc`, `api_handlers.cc`, `cache_handlers.cc`,
`capture_handlers.cc`, `websocket.h/.cc`,
`ws_handlers.cc`, `static_file_handler.cc`

**CLI flags**: `--api-port PORT` (default: 0=disabled), `--api-token TOKEN`
(or `PAGESPEED_API_TOKEN` env var), `--api-read-open` (or `PAGESPEED_API_READ_OPEN=true`),
`--console-dir PATH`

**Auth**: Bearer token via `--api-token` flag or `PAGESPEED_API_TOKEN` env var.
Health and the `/console`/`/console/*` static bundle
(GET/HEAD only — static assets carry no data, #1449) are always exempt.
`--api-read-open` is an explicit allow-list, not a default: `RouteAuth::kToken`
is the default for every route (token required on TCP regardless of read-open),
and a GET/HEAD route opts in with `RouteAuth::kReadOpenOk` (`/v1/stats`,
`/v1/metrics`, `/v1/config`, and the four `/v1/cache/*` read routes today).
A route left at the default — including one added later — stays closed under
read-open. The optimizer log is the one GET route that deliberately does not
opt in: `GET /v1/logs` keeps the token on TCP, and so does the `/v1/ws/logs`
stream (`WsConfig::read_open_streams`, default `{"stats", "events"}`, is the
matching allow-list on the WS side) — not on the handshake (a browser cannot
carry `Authorization` there), but via the same in-band auth message every
WebSocket stream already uses. The unix socket and `--api-no-auth` leave both
open, since `HttpServer::CheckAuth`/`WsManager::AcceptUpgrade` return before
ever consulting `RouteAuth`/`read_open_streams` on those paths.
Mutating endpoints (POST/PATCH/DELETE) always require auth when a token is set.
This enables exposing the console publicly as a read-only demo while requiring
a token for configuration changes. WebSocket uses auth-via-first-message
(not query param) when read_open is disabled.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/health` | GET | Health + readiness (no auth) |
| `/v1/stats` | GET | Full JSON statistics |
| `/v1/metrics` | GET | Prometheus text format |
| `/v1/config` | GET/PATCH | Read/update hot-reloadable config |
| `/v1/cache/alternates` | GET | List variants for URL |
| `/v1/cache/urls` | GET | Paginated cached URL list |
| `/v1/cache/select` | GET | Select best alternate for mask |
| `/v1/cache/content` | GET | Raw alternate content |
| `/v1/cache/purge` | POST | Purge URL variants; full-cache purge with `{"scope":"all","confirm":"purge-all"}` |
| `/v1/cache/reprocess` | POST | Purge + re-optimize |
| `/v1/cache/cooldowns` | GET | List active HTML processing cooldowns |
| `/v1/capture/waterfall` | POST | CDP network waterfall (needs Chrome) |
| `/v1/capture/screenshot` | POST | CDP screenshot (needs Chrome) |
| `/v1/ws/stats` | WS | Live stats streaming |
| `/v1/ws/events` | WS | Real-time event stream |
| `/v1/logs` | GET | Recent log entries from the in-memory ring (`since`/`limit` cursor; pages ≤ 512 KiB; `stream_id` changes on restart); token required on TCP even with `--api-read-open` |
| `/v1/ws/logs` | WS | Real-time log streaming (ring buffer snapshot + live); token required on TCP even with `--api-read-open` (sent as the first message, not the handshake) |
| `/console/*` | GET | Static file server for web console |

**Tests**: `test/src/worker/` — 23 test files, key ones:
`http_server_test.cc` (105), `cache_handlers_test.cc` (106),
`capture_handlers_test.cc` (129), `image_transcoder_test.cc` (272),
`worker_test.cc` (198), `api_handlers_test.cc` (25), `websocket_test.cc` (55).
SVG pipeline tests in `test/lib/image/`: `svg_vectorizer_test.cc` (33),
`svg_sanitizer_test.cc` (67), `svg_preprocessor_test.cc` (27),
SVG candidacy in `content_analyzer_test.cc` (56).

## Management Socket

Listens on `{socket_path}.mgmt`. Newline-terminated text commands.

| Command | Description |
|---------|-------------|
| `AUTH <token>` | Authenticate for PURGE (requires `PAGESPEED_PURGE_TOKEN` env var) |
| `PURGE <hostname> <url>` | Delete all variants for URL (auth required) |
| `STATS` | JSON stats with counters, timing, cache stats |
| `METRICS` | Prometheus text exposition format |
| `BROWSER-STATUS` | JSON browser analysis status (Chrome state, queue, profiles) |

## Notification Deduplication

Uses an **in-memory processed set** (`processed_variants_`) to track which
`(url, hostname, alternateId)` combinations the worker has already written.
On successful variant write, the key is added to the set.  On the next
notification for the same combination, processing is skipped.

Uses an in-memory set instead of cache reads because Cyclone's write-around
RAM cache populates on reads but does NOT evict on writes.  A `ReadAlternate`
dedup check would pollute RAM with stale content that shadows the worker's
subsequent write.

`kFlagWorkerProcessed` metadata flag (bit 1) is still set on all worker-written
variants for future use (e.g., cross-process dedup after restart).

Variants written with `kFlagNeedsRevalidation` are NOT added to the processed
set, allowing re-processing once external CSS becomes available.  The set is
cleared on URL invalidation (purge) and bounded to 10,000 entries.

**Image incomplete-matrix gap-fill** (`image_incomplete_retries_`): When the
proactive image loop writes fewer variants than expected (e.g., due to cache
write failures from race conditions with nginx), the URL is NOT marked as
processed. This allows re-notification to fill the gaps. The loop's
`HasVariant()` check naturally skips existing alternates, writing only missing
ones. Capped at `kMaxIncompleteRetries` (3) to prevent infinite loops on
persistent failures. Counter: `image_incomplete_matrices`.

**Text processing cooldown** (`text_cooldown_expiry_`): Prevents tight
re-processing loops for HTML and CSS.  Map stores expiry times keyed by
`url|hostname`, bounded to 256 entries.  Three cooldown durations:
- **Tentative** (60s): Set before processing starts, prevents concurrent
  threads from processing the same URL.  Cleared on non-revalidation success.
- **Write failure** (60s): Reader contention on mmap handles.
- **Revalidation** (3s): Successful write with `kFlagNeedsRevalidation`.
  Throttles the CSS convergence loop while allowing timely re-processing
  once external stylesheets become available.

## HTML Processing

Two-pass pipeline: HtmlScanner (pass 1, metadata) → HtmlTransformFilter +
HtmlWriterFilter (pass 2).

**HTML convergence**: First processing may lack cached CSS. Variant written with
`kFlagNeedsRevalidation`. Nginx re-notifies on HIT. Re-processing injects critical
CSS once available, clears flag. All transforms are idempotent.

### Transforms (all in `HtmlTransformFilter`)

1. **Critical CSS injection** — Heuristic-based, viewport-aware `@media` filtering
   (px/em/rem, comma-separated query lists, conservative fallback for unrecognized).
   CSS selector escape handling: backslash-literal (`\:`) and hex (`\3a`) per
   CSS Syntax Module Level 3 §4.3.11. XSS prevention: null-byte stripping then
   `</style` check. Always includes `*`, `html`, `body`, `:root`, the first 300
   elements inside `<body>`, position:fixed elements and their subtree,
   for mobile/tablet the rules of replaced elements that may be wider than a
   phone (`img`, `iframe`, `video`, unsized `svg`, ... without a markup width
   <= 320 px; one anywhere can widen the document; at most
   `kMaxWideReplacedBytes` = 8 KiB and never past the worker's inline limits),
   header/nav/hero patterns. Excludes footer/lazy/below-fold, depth > 10,
   `@media print`. Inside `@layer`, large `@media` blocks (>4KB) are filtered
   per-rule instead of included wholesale (`max_wholesale_media_bytes`).
   See `src/worker/critical_css_extractor.h`.
   **Placement**: before the page's first stylesheet source (an
   author's loadCSS preload counts), so the full sheets win every tie. A block that names a cascade layer goes there
   only with an `@layer …;` statement in front listing the page's layers in
   document order, computed in `BuildCombinedCss` from
   `HtmlScanResult::stylesheet_sources` (`layer_order_sources.h`,
   `cascade_layer_order.h`) and refused (old placement before `</head>`) when a
   source is not gathered or titled, a layer is first declared under a
   condition, an anonymous layer precedes a named one, the CSS has a stray
   `}`/`;` or an escaped at-keyword, or a script that may insert a sheet comes
   before a sheet declaring a new layer. The sources are exactly the sheets
   the combined CSS reads (the scanner's stylesheet-source rule: `StyleScope`, the `type`
   checks, the loadCSS preload at its own position), plus links a browser
   applies that the gather does not read, which leave the order unproven. On
   a layered sheet the validation record binds to the order
   (`ValidationBinding`, folded into the combined-sheet hash, so a change
   triggers `RequestRevalidation`). The validator applies the same rule
   (`DecideCriticalCssLayerPlacement`) with the same order. `@media`
   retention follows placement: the device class's window range
   (`RetentionWidthRange`) when the block goes first and the CSS has no
   anonymous layer (`CriticalCssRetentionMayNarrow` on the combined sheet,
   confirmed on the NUL-stripped finished block; an anonymous layer is a
   separate, earlier layer, which wins for `!important`), every width
   otherwise; the binding then reads `proven r2 <statement>`. Inside an
   anonymous layer the block carries no `!important` declaration (the
   block's anonymous layer comes before the sheet's, and the earlier layer
   wins for `!important`; moving the block after the sheet would make its
   later layer win normal declarations instead), and pages whose sheet may
   have one bind with an `a2` marker (`proven a2 …` / `unproven a2`).
   A block that goes after the sheets carries no anonymous layer at all
   (there its anonymous layer comes after the sheet's and wins normal
   declarations); decided from the combined sheet
   (`CriticalCssBlockGoesFirst`) and checked on the finished block, binding
   marker `u2`. An at-rule keyword written with an escape (`@l\61yer {`) is
   a possible anonymous layer on both sides (`CssTextHasAnonymousLayer`,
   `CssAtRuleKeywordHasEscape`; the placement side never decoded it either),
   so such a rule is left out whole of a block that goes after the sheets, and
   the record's binding is also salted with the derivation's
   `CriticalCssResult::anonymous_layers_dropped` (`ValidationBindingFor`,
   `anon-layers-dropped`), which covers a block that only the finished-block
   check moves after the sheets. The full binding grammar is documented at
   `CascadeLayerOrder::ValidationBinding`. The order is passed to
   `CriticalCssExtractor::Extract` / `DeriveDomMatchedCriticalCss`
   on the serve and validation paths alike.
2. **LCP preload** — `<link rel="preload" as="image" fetchpriority="high">` in
   `<head>` for LCP candidate detected by HtmlScanner. Includes `imagesrcset` and
   `imagesizes` when available. URL scheme validation rejects `javascript:`/`data:`.
   Also written to Early Hints sentinel with `image:` prefix. Both preloads are
   suppressed for an `<img>` inside `<picture>` (a `<source>` sibling may win
   selection — the preload could double-download); the `<img>` itself still gets
   `fetchpriority="high"`, which stays correct whichever source wins. An
   `<img>` inside `<noscript>`, `<template>`, `<noembed>` or `<noframes>` is
   never the candidate (a browser running scripts does not create it,
   so the preload and Early Hint would fetch an image the page never shows;
   the scanner's `inert` rule, the same one that keeps such elements out of
   `elements`). It still advances `element_index`, so
   `lcp_candidate.element_index` stays comparable. That scope is the lexer's
   markup parse of `<noscript>` (`kSometimesLiteralTags` in
   `lib/html/html_lexer.cc`), not the browser's RAWTEXT "first `</noscript>`
   byte sequence" rule, so they diverge on a nested `<noscript>`, a
   `</noscript>` inside a comment or script string within the noscript, and a
   `<noscript>` inside `<svg>`; where they differ the scanner's scope is the
   longer one, so an image the browser does create is skipped and a later
   real image is the candidate (less harmful than the earlier preload of a
   URL the browser never fetches). A **script-loaded hero** yields no
   candidate at all (`LcpCandidate::script_loaded_hero`): the first
   hero container (`IsHeroContainer`: `<header>`/`<main>`/`<section>`/
   `<article>` or a hero class) holds a lazy-load placeholder
   (`IsLazyLoadPlaceholder`: no usable `src`, plus a `data-*src*`/
   `data-original` attribute or a `lazy` class), followed by its `<noscript>`
   copy, and no eligible image of its own. Chromium reports the loader-filled
   `<img>` as the LCP, but the markup does not say which bytes the loader
   fetches (`data-src` is a convention; the twin's URL may differ in size or
   format), so nothing is hinted: no preload, no Early Hint, and no
   `fetchpriority` fallback either, rather than hinting the next image, which
   is probably below the fold. The decision drops an earlier fallback and
   rules out every later image except one in a hero container opened after
   the decision (`open_hero_containers_` marks each open container with
   whether it opened after the decision; the innermost one must be fresh,
   whatever its depth, so a sibling shallower than a nested decided
   container counts), so a lazy logo with its copy alone in `<header>` does
   not silence the real hero in the `<section>` after it, while an image
   elsewhere inside the `<main>` that wraps the lazy hero stays unhinted;
   such a later hero image withdraws the decision. "A later sibling
   section" includes a content `<section>`/`<article>` below the fold: an
   image in a later section inside the wrapping `<main>` is hinted, as
   before this rule. A
   placeholder outside a hero container, without the copy, in a container
   that also holds an eligible image, or whose only hero class is its own
   (no ancestor container to hold the copy) changes nothing. Two
   refinements. **Stand-in `src`**: the placeholder may carry a
   real-looking `src`. One whose file name is a common stand-in
   (`IsLazyStandInSrc`: blank, placeholder, pixel, spacer, 1x1,
   transparent, loading, lazy, optionally with a size or number, as the
   whole name after splitting on `-`/`_`/`.`; `/blank.gif`,
   `/placeholder-300x200.png`) next to a lazy data attribute
   (`HasLazySourceAttribute`) is a placeholder outright, twin or not, in or
   out of a hero container. Any other `src` with a lazy data attribute is
   taken as the hero candidate as before, provisionally
   (`lazy_hero_stand_in_src_`): if its `<noscript>` copy in the same hero
   container carries a DIFFERENT `src`, the copy says what the real image
   is, the candidate is withdrawn and the `<img>` is the pending
   placeholder from then on (`<img src=/hero-lqip.jpg data-src=/hero.jpg>`
   + `<noscript><img src=/hero.jpg>` gives no candidate); no copy, or a
   copy with the same `src`, and it stays the candidate. "Same" is the
   resolved reference (`ImageIdentity`, SheetIdentity's reduction: host,
   path and query, scheme and fragment dropped; a reference without a host
   takes the base URL's (`BaseOf`, shared with the stylesheet matching in
   `EndDocument`), so under a cross-host `<base href>` a
   root-relative src is the CDN's image, as the browser resolves it, while
   a root-relative `<base href="/sub/">` keeps the page's host), so
   `http://example.com/hero.jpg` is `/hero.jpg` while `/hero.jpg?v=2` is
   not known to be; percent-encoding and an explicit default port are
   compared literally. In every leg the copy has to be plausible: a
   `<noscript>` image that `IsUnlikelyLcpImage` (a 1x1 tracking pixel's
   no-JS fallback in the same `<main>`, a `hidden` image) confirms nothing,
   for the src-less placeholder too (a placeholder whose only copy
   is a pixel is handled as one without a copy). That test is
   markup-declared only, so a 2x2 `<noscript>` image, or one hidden by a
   CSS class, still counts as a copy; real tracking pixels are 1x1 and
   attribute-hidden, so this is accepted. A lazy class alone never
   makes a real `src` a stand-in (`<img src=/hero.jpg class=lazy>` is the
   hero, and so is `<img src=/blank.gif class=lazy>` whatever its copy
   says: the rule asks for the data attribute); outside a hero container
   the twin leg does not apply (the fallback candidate is not retracted).
   Scanner/filter asymmetry, pre-existing: the scanner's leg 1
   does not read `srcset`, the filter's does, so `<img src="data:..."
   srcset=/hero.jpg data-src=/x>` is a placeholder for the scanner and a
   loading image for the filter. **Small icon**: an eligible image
   inside the pending placeholder's container whose declared size is small
   (`IsSmallDeclaredImage`: inline `width:`/`height:` px or the attributes,
   both axes, product under `kSmallImageAreaPx2` = 10,000, so below
   100x100; unknown is not small) is set aside instead of taken
   (`deferred_small_`): it cancels nothing, and with the copy seen it is
   dropped, so the page gets no candidate rather than the 48x48
   call-to-action icon. Without the copy the pattern is not confirmed and
   the icon is the candidate, as the first-eligible-image rule picked it
   before (also when a larger image of the container's own follows); a page
   without a placeholder is untouched: an icon that is its first eligible
   image is the candidate exactly as before. The threshold is the only area
   rule in the heuristic (`IsUnlikelyLcpImage` rejects 0/1 px and hidden
   only) and is used nowhere else. The tolerance applies while a
   placeholder is pending: an icon that PRECEDES the placeholder in the
   container is the first eligible image and the candidate, as before; and
   a second placeholder+copy in a fresh container after a decision does not
   start a new pattern (as before), so an icon there is that container's
   candidate.
   Disable: `--no-lcp-preload`.
3. **Lazy load** — `loading="lazy"` on `<img>`/`<iframe>`. LCP candidate image gets
   `fetchpriority="high"` instead; without a candidate, the first *plausible* body
   image within the first-3-imgs window is promoted (none if no image qualifies).
   Implausible-LCP images (`IsUnlikelyLcpImage`: 0/1px dimensions, `hidden`,
   `display:none`/`visibility:hidden`) are never promoted AND never lazy-loaded
   at any position — lazy on a layout-less beacon makes browsers skip the load,
   breaking analytics. First 3 body images are not lazy-loaded when LCP candidate
   is set (above-fold guard); the first VISIBLE body iframe is exempt (likely
   above-fold embed). Invisible iframes (`IsInvisibleElement`, same evidence as
   images — the GTM-noscript tracking-frame shape) get no transform at any
   position and do not consume the exemption slot. An `<img>` or `<iframe>`
   inside `<noscript>`, `<template>`, `<noembed>` or `<noframes>`
   (`InsideInertSubtree`) gets no transform either and consumes no img or
   iframe slot: a scripting browser never creates it, and for a
   no-JS client it is the author's fallback, left as written. An `<img>`
   without a usable source (no `src`, empty or `data:`, or a stand-in name
   such as `/blank.gif` next to a lazy data attribute; and no
   `srcset`, nor a `<source srcset>` sibling in `<picture>`) is a lazy-load
   placeholder the author's loader fills in: it never takes the
   fetchpriority fallback, is left as written inside the 3-image
   window, and is lazy-loaded past it like any image; it does count toward
   the window. The filter cannot see a `<noscript>` copy that comes after
   the `<img>`, so a real `src` with a lazy data attribute is an image that
   loads here; the scanner's twin leg reaches the filter as
   `script_loaded_hero`. With
   `lcp_candidate.script_loaded_hero` set nothing is promoted and the window
   guard applies as with a named candidate. Disable:
   `--no-lazy-load-images`.
4. **Image dimensions** — Injects `width`/`height` from cached image headers (no full
   decode). Disable: `--no-image-dimensions`.
5. **Preconnect injection** — Detects third-party origins from external resources
   and writes `preconnect:` (no-cors) or `preconnect-cors:` (CORS-mode) prefixed
   hints to the Early Hints sentinel, enabling nginx to emit
   `Link: <origin>; rel=preconnect` headers. The injected HTML preconnect carries
   `crossorigin` exactly when the first-seen resource for that origin fetches in
   CORS mode (explicit `crossorigin` attribute, ES module script, font preload) —
   browsers key connection reuse on request mode, so the warmed pool must match.
   Disable: `--no-preconnect-injection`.
6. **Async CSS loading** — Makes render-blocking `<link rel="stylesheet">`
   elements non-blocking by switching each to `rel="preload" as="style"`
   (original media recorded in `data-pagespeed-media`, live `media` dropped —
   on a preload it is a fetch condition, not applicability). A CSP-safe
   external loader flips `rel` back to `stylesheet` and restores the media once
   the sheet has arrived. `as="style"` is load-bearing twice over: it gives the
   fetch the priority a stylesheet deserves (`media="print"` earned Lowest),
   and it is what the browser matches the preload against when the loader turns
   THIS SAME element into its consumer — a mismatch downloads the sheet twice,
   so the loader never creates a second `<link>`. Being a real preload also
   makes the sheet Early-Hints eligible again (see the interplay note below).
   The transform injects ONE external
   `<script defer src="/pagespeed_static/async_css.<hash>.js" data-pagespeed-async-loader>`
   per document, where `<hash>` is content-derived (FNV-1a) from the loader body
   so a loader change yields a fresh, immutable-cacheable URL (single source of
   truth: `async_css_loader.{h}` =
   `kAsyncCssLoaderPath`/`kAsyncCssLoaderJs`; served by the nginx module and the
   .NET middleware). No inline `onload`, so it works under `script-src 'self'`.
   A `<noscript>` fallback (marked `data-pagespeed-async-fallback`, carrying
   `integrity`/`crossorigin`) covers non-JS clients. The inlined critical CSS
   paints first. Applied whenever critical CSS is injected AND it is sufficient
   to bridge first paint (gate: `!disable_async_css && has_critical_css &&
   CriticalCssIsSufficient(...)` — the pessimistic of the extracted-bytes
   ratio and the browser profile's rule-level coverage (when known) must be
   ≥ `async_css_min_coverage` (default 0.10; profile coverage is only trusted
   downward — it can over-report vs. the bytes actually inlined, the #879
   FOUC), unless the deferred sheet is below
   `async_css_min_deferred_bytes` (default 15000). Below the floor, async-CSS is
   suppressed — the critical CSS is still inlined but the stylesheet stays
   render-blocking, avoiding a flash of unstyled content; `async_css_suppressed_low_coverage`
   counts these. The policy `async_css_recommended` signal is telemetry, NOT
   consulted by the gate). **Cold-cache fail-safe:** when a declared
   external stylesheet was not resolved from cache (`external_css_missing`),
   `CriticalCssIsSufficient` returns false unconditionally — the deferred-byte
   denominator is then the inline-only blob and the critical CSS was derived
   without the sheet, so the small-sheet escape hatch would otherwise async-defer
   an unmeasured sheet (the iispeed.com FOUC). The variant is marked
   `kFlagNeedsRevalidation` when a **same-origin** external sheet is missing (NOT
   gated on `!critical_css_injected()`, which a degenerate inline echo would
   trip; and NOT for cross-origin sheets such as Google Fonts, which can never
   enter the cache and would otherwise re-notify forever), so the decision
   self-heals once a cacheable sheet caches. The async-CSS *suppression* itself
   still keys on any `external_css_missing` (cross-origin included), so those
   pages stay render-blocking and FOUC-free — they just never upgrade to async.
   **Empirical gate (the byte floor is necessary, not sufficient):** the ratio
   above is a proxy — it can clear while the inlined block still fails to cover
   the fold. Deferral additionally requires a POSITIVE validation record on the
   viewport profile the critical block was derived from, and that record must
   still be bound to the stylesheet being served:
   `critical_css_validated && validated_combined_css_hash ==
   CombinedCssValidationHash(combined_css)` (`AsyncCssValidatedForServedSheet`).
   Consequences, all deliberate: the **heuristic path never defers** (no
   profile, no record, and none obtainable); a **stylesheet-only redeploy**
   invalidates the record even though `TemplateDetector::HashStructure` covers
   only the tag tree and cannot see it; and an **empty combined sheet** hashes
   to nothing matchable, so the `deferred_css_bytes == 0` free pass can no
   longer defer a real `<link>` against a decision made from no stylesheet at
   all. Async-CSS is therefore a warm-cache, analyzed-page optimization: a page
   whose analysis has not completed keeps its stylesheet render-blocking, with
   the critical CSS still inlined. `async_css_suppressed_unvalidated` counts
   these refusals, separately from the low-coverage counter, because the
   operator action differs. `--unsafe-force-async-css` (CLI-only, diagnostic)
   bypasses the whole gate by design.
   **A record covers ONE block (issue #1216):** the accept test reads the
   profile's bit and the stylesheet hash, and neither says which critical block
   is being inlined. When `DeriveDomMatchedCriticalCss` comes back EMPTY for the
   page being served, the block the record was produced about is not the block
   that ships — the heuristic fallback substitutes its own, or nothing is
   inlined — while the record still reads as healthy. `AsyncCssRecordForDerivedBlock`
   therefore drops the record at the derivation site, before the gate ever sees
   it (`async_css_record_dropped_empty_derivation` counts the drops). The byte
   floor cannot stand in for this: under `async_css_min_deferred_bytes` the
   small-sheet escape hatch clears whatever the coverage is, and an empty
   derivation pins `critical_css_coverage` at 0 for the substituted block
   anyway. Today's two extractors happen to make the substitution unreachable —
   the fallback matches the same DOM through a strictly less inclusive matcher,
   so an empty derivation implies an empty fallback — but that is a property of
   the two matchers, not of the gate, and the gate is where it has to hold.
   **Who produces the record** (`src/browser/critical_css_validator.{h,cc}`,
   driven from `BrowserAnalysisManager::RunCriticalCssValidation`): during
   analysis, per viewport, the manager derives the candidate block with the
   SAME `DeriveDomMatchedCriticalCss` the serve path inlines, synthesizes two
   documents from the page's PRE-INLINE markup (every stylesheet `<link>` and
   every `<style>` stripped, then exactly one injected — the reference gets the
   full combined sheet, the candidate gets the critical block, and the pair is
   checked to differ nowhere else), and renders both through
   `VisualRegressionGate::Compare`. Diff ≤ `kDefaultValidationDiffThreshold`
   (0.005, measured — see the constant's comment) stamps the record.
   Three things are load-bearing and easy to break:
   (a) the **pre-inline** HTML — `AnalysisContext::html_content` has the sheet
   inlined for coverage, so building the candidate from it would hand the
   candidate the whole sheet and validate everything;
   (b) the combined sheet comes from `Worker::BuildCombinedCss` through the
   injected `set_combined_css_builder` seam, never a second assembly, or every
   record fails the serve-time hash comparison and the feature is inert; and
   that assembly, like the analysis-side CSS inliner, resolves each `<link>`
   href through `DocumentBaseOf` / `ResolveAgainstBase` (`html_scanner.h`):
   the page URL or its first `<base href>`, with a hostless href
   fetched from the `<base>` host when that is not the page's own, so under a
   cross-host `<base>` the CDN's sheet is gathered on both sides instead of
   counting as missing on both (the worker's page URL is the cache-normalized
   path, so the page's own host and scheme come from the notification or the
   queue item);
   (c) every failure path (no Chrome, injector abort, screenshot failure,
   oversized document, a comparison over zero pixels) records NOTHING, and
   `ApplyValidationVerdict` clears before it writes.
   Cost: 2 renders per viewport, 6 per template, on templates that reach the
   gate. Skipped when `ShouldInlineCriticalCss` already suppresses, when no
   block was produced, and when `external_css_missing` (the record could not
   match at serve time anyway).
   THREE blind spots, all named in `critical_css_validator.h`: (1) JS-off and
   (2) all-subresources-blocked are the validation render's SSRF defense, so
   both documents lose them equally — only the rendered probe lane
   (`tools/async-css-probe`) covers that class; (3) the record hangs off the
   TEMPLATE profile, whose hash ignores classes/ids, so a sibling page defers on
   another page's confirmation against a block re-derived for its own DOM.
   Deliberate (plan Q6: per-page hashing would invalidate nearly everything),
   unmitigated, and the reason `--no-async-css` stays one command away.
   LIFETIME: `validation_gate_` is a manager member destroyed BEFORE `chrome_`
   in both `OnChromeExit` and `Shutdown`. An in-flight comparison holds a raw
   `CdpClient*` and a 60 s uv timer, and once the client dies that timer is the
   only path that can complete the capture — reverse the order and it fires
   into freed memory on the COMMON path, not a rare one.
   The shared `CriticalCssIsSufficient` is also called by the in-process
   `ps_html_process` (.NET) path for parity — **but the empirical gate is not**,
   and cannot be: that path has no browser and no profile, so it can never
   produce a validation signal. `ps_html_process` keeps its byte-ratio floor
   plus the `external_css_missing` fail-safe, and its `enable_async_css`
   defaults to off, so an embedder turning it on has made a deliberate choice.
   The lower-level `ps_html_transform_create` had no sufficiency gate at all and
   now ignores the flag outright (see `lib/pagespeed/pagespeed.cc`).
   **CSP/inline coupling:** deferral is only safe because the inlined block
   paints the fold meanwhile, so when the inline injection is REFUSED (a
   restrictive meta CSP, or a `</style` terminator in the block) the filter
   reverts every conversion it made in that pass — `HtmlTransformFilter::
   RevertAsyncCss` at `EndDocument`. The `MetaCspAllowsInlineStyle()` check at
   `StartElement` is only an early-out: a `<link>` in source order BEFORE the
   CSP `<meta>` is converted without having seen that policy.
   Revalidation:
   `RemoveAsyncCssMarkers` restores `rel="stylesheet"` + media, drops `as`,
   and strips the markers; the old `<noscript>` fallback and loader `<script>`
   are deleted and re-injected. Restoring `rel` is the load-bearing half: a
   link left at `rel="preload"` with the loader gone downloads the sheet and
   applies nothing. `HtmlScanner` matches the deferred primary on
   `rel="preload"` + `data-pagespeed-async` (not on `rel=preload` alone, which
   would swallow LCP/font preloads) and reports its RECORDED media, so the
   stylesheet list is the same on the raw and reprocessed passes.
   Early Hints interplay: stylesheets the transform defers ARE preload-hinted
   — the primitive is itself a preload, so the hint announces exactly the
   fetch the markup announces, only earlier (`ParseHintLine` renders a bare
   sentinel line as `rel=preload; as=style`). Accepted cost: at style priority
   the sheet competes with the LCP image hint. `media="print"` sheets are
   always omitted. When reprocessing leaves the hint list empty, the stale
   sentinel is removed rather than left behind — otherwise the 103 path would
   keep promoting a resource the page no longer wants.
   Disable: `--no-async-css`.
7. **Script deferral** — Adds `defer` to `<script src="...">` identified as safe
   by browser script analysis. Evidence-based: only scripts the analysis browser
   actually fetched (same-origin, present in cache, served via Fetch
   interception) and observed idle before FCP earn a defer verdict; a script
   the analysis could not observe classifies `kNoCoverageData` and is never
   deferred. Path-boundary suffix matching. Skips scripts with
   `async`, `defer`, or `type="module"`. Revalidation: strips `data-pagespeed-defer`.
   Disable: `--no-script-deferral`.

## CSS Processing

Read CSS → `FlattenImports()` → `MinifyCss()` → write variant.
`@import` flattening inlines cached dependencies (depth limit 5, circular detection,
relative URL resolution, media query wrapping). Disable: `--no-css-import-flattening`.
Flattening is all-or-nothing per sheet: any uninlinable import → the original is
minified instead, and since CSS variants have no revalidation (only HTML does),
the skip heals only at TTL expiry/purge. See `lib/css/css_import_flattener.h`.
Import-free CSS and JS record a content hash and restamp on a matching hash after a
re-record; flattened sheets (and sheets with a skipped import) record none and still heal only at TTL/purge.

## Pre-Compressed Text Variants

After writing an identity (uncompressed) text variant (HTML, CSS, or JS), the worker
produces up to 2 additional alternates: gzip and brotli. This enables the nginx module
to serve pre-compressed content without on-the-fly compression overhead.

- Controlled by `--gzip-level N` (1-9, default 6, 0=disable) and
  `--brotli-level N` (1-11, default 6, 0=disable).
- Worker compresses at level 6 (offline, higher quality). Nginx's dynamic
  compression fallback uses level 4 (latency-sensitive). This means
  pre-compressed variants are smaller than nginx's on-the-fly fallback.
- Images are excluded (already format-compressed).
- For CSS/JS where minification doesn't reduce size ("already minimal"), compressed
  variants of the original content are still written.
- `origin_content_type` from the original alternate's metadata is propagated to
  compressed variants so nginx can set the correct `Content-Type` header
  (critical for extensionless HTML URLs like `/` or `/about`).
- Implementation: `WriteCompressedVariants()` helper in worker.cc,
  `text_compressor.{h,cc}` for compression primitives.

## Proactive Image Variants

Single decode → encode to all missing format/viewport/density/save-data combinations.
Up to 36 variants per notification (3 formats x 3 viewports x 2 densities x 2
save-data). Disable individually: `--no-proactive-image-variants`,
`--no-proactive-viewport-variants`, `--no-proactive-savedata-variants`,
`--no-proactive-density-variants`.

Incomplete matrices (fewer variants written than expected) are not marked as
processed, allowing re-notification to fill gaps. Counter:
`image_incomplete_matrices` (in STATS/METRICS). Retry cap: 3 attempts per URL.

## CLI Flags Reference

**Core**: `--num-threads N` (default: auto, clamped to [2, 128]),
`--cache-size BYTES` (default: 1GB)

**Content-type toggles**: `--html-mode MODE` (`off`, `markdown-static`,
`markdown-rendered`, `full`; default `full`), `--disable-html` (same as
`--html-mode off`; `--html-mode` wins when both are given), `--disable-css`,
`--disable-js`, `--disable-image`. The `markdown-*` modes write no optimized
HTML variant for any sender. `pagespeed.json` and `PATCH /v1/config` take
`html_mode` and the legacy `disable_html` under the same rule (an explicit,
valid `html_mode` wins; a disagreeing `disable_html` in the same body is
ignored with a warning); both are written back, `disable_html` derived
In a `markdown-*` mode the raw entry is still stamped (derived ETag), and a
repeat notification for an unchanged page (same origin hash, content-hash
record present) is skipped for 10 minutes (`markdown_pass_last_`); an origin
change, purge or refresh always processes. On the nginx front both
`markdown-*` modes currently build the agent copy the same way, and variants
stored under `full` stay servable until the page changes, refreshes or is
purged

**Compression**: `--gzip-level 6` (0=disable, 1-9), `--brotli-level 6`
(0=disable, 1-11)

**Quality**: `--jpeg-quality 85`, `--webp-quality 75`, `--avif-quality 60`

**Save-Data quality**: `--savedata-jpeg-quality 60`, `--savedata-webp-quality 50`,
`--savedata-avif-quality 45`

**Viewport widths**: `--mobile-width 480`, `--tablet-width 768`,
`--desktop-width 0`

**Security limits**: `--max-url-length 8192`, `--max-html-size 5MB`,
`--max-css-size 2MB`, `--max-js-size 2MB`, `--max-image-size 10MB`

**Logging**: `--log-level debug|info|warning|error`, `--log-format text|json`

**Browser analysis**: `--enable-browser-analysis` (off by default),
`--chrome-binary PATH` (default: /usr/bin/chrome-headless-shell),
`--chrome-recycle-interval N` (default: 100),
`--chrome-page-timeout MS` (default: 60000),
`--chrome-max-memory MB` (default: 512),
`--chrome-startup-timeout MS` (default: 10000),
`--no-browser-critical-css`, `--no-browser-lazy-loading`,
`--no-browser-lcp-preload`, `--no-browser-image-sizing`,
`--browser-queue-size N` (default: 1000),
`--browser-profile-ttl SECONDS` (default: 86400; while same-origin mappable
scripts are still cache-cold the effective TTL is clamped to
min(configured, 3600s) so the template re-analyzes once the cache warms —
the cold-script heal)

**SVG auto-vectorization**: `--svg-mode detect` (detect|preview|auto),
`--svg-candidacy-threshold 50`, `--svg-max-pixels 65536`,
`--svg-max-paths 500`, `--svg-fidelity-threshold 55.0`,
`--svg-exclude-lcp true`, `--svg-timeout-ms 500`,
`--svg-preset 1` (0=bw, 1=poster, 2=photo),
`--svg-color-precision 0` (0=adaptive, 1-8=fixed),
`--svg-filter-speckle 4`

**Outbound traffic**: the daemon makes no request of its own accord — no license
renewal, no heartbeat, no telemetry. The only egress is
operator-configured: origin content fetches and the Web Bot Auth / RSL-CAP JWKS
key directories named on the command line.

**Sentinel constants**: `kWarmupSentinel = 0xFFFFFFFE` (capability_mask.h),
`kLlmsTxtSentinel = 0xFFFFFFFD` (capability_mask.h),
`kOriginRefreshedSentinel = 0xFFFFFFFC` (capability_mask.h, issue #652 —
nginx re-fetched expired origin content and re-recorded the fresh body at
the identity id; the worker purges the stale NON-identity variants,
preserves the fresh identity, clears dedup, and rebuilds the variant set
inline; rate-limited per URL),
`SentinelId::kEarlyHints` (alternate_id.h),
`SentinelId::kBrowserProfile` (alternate_id.h).

## Browser Analysis Integration

When `--enable-browser-analysis` is set and Chrome is available, the worker uses
headless Chrome (CSS Coverage API + PerformanceObserver) to generate per-template
`OptimizationProfile` data, replacing heuristic critical CSS with browser-validated
results.

### Architecture

`BrowserAnalysisManager` owns Chrome lifecycle, analysis queue, and the CDP pipeline.
It runs on the main libuv event loop (where CDP must operate). Worker thread pool
threads enqueue analysis requests via `uv_async_send()`.

### Flow

1. Worker thread: `HtmlScanner::Scan()` succeeds
2. Worker thread: `TemplateDetector::HashStructure()` computes template hash
3. Worker thread: `LookupProfile()` checks cache for existing profile
4. If profile found: use browser-validated critical CSS instead of heuristic
5. If no profile: `EnqueueAnalysis()` + `uv_async_send()` to main loop
6. Main loop: `DrainQueue()` → `RunAnalysis()` → CSS cache inlining, `<base
   href>` injection (absolute page URL, skipped when the author set one) and
   the analysis resource map build (same-origin cached script bytes, served
   to Chrome via Fetch interception) → CSS extraction + page analysis for all
   3 viewports (Mobile 375x667, Tablet 768x1024, Desktop 1440x900) → script
   coverage pass (Desktop). Mapped scripts EXECUTE during page analysis, so
   LCP/CLS/fold telemetry comes from a script-executed render (intended
   behavior change)
7. Profile stored in cache at `OptimizationProfile::CacheUrl(hash)` with
   `SentinelId::kBrowserProfile`

### Error Handling

All failures fall back to the heuristic path. Browser analysis is strictly additive:
- Chrome won't start → heuristic only, retry after 2s
- Chrome crashes → cancel analysis, restart after 2s
- Analysis timeout → skip item, process next
- Cache read fails → skip item
- Queue full → head-drop oldest item

### CSS Cache Inlining

Before passing HTML to Chrome, `InlineCachedStylesheets()` (in
`css_cache_inliner.h/.cc`) resolves `<link rel="stylesheet">` tags against
the Cyclone cache and injects `<style>` blocks into the HTML. This enables
Chrome's CSS Coverage API to compute real coverage instead of returning 0%.

Pipeline: HtmlScanner -> cache lookup -> FlattenImports -> XSS sanitize -> inject.
When flattening skips (all-or-nothing), the original sheet — imports intact — is
inlined for the analysis browser, which cannot fetch them: coverage may
under-measure in mixed-cache states until the import tree is fully cached.

Each href resolves against the document base, the one rule `BuildCombinedCss`,
the stylesheet Early Hints, the SRI pins and the scanner's sheet and image
   identities share (`DocumentBaseOf` / `ResolveAgainstBase` in `html_scanner.h`):
   the page URL, or the first `<base href>` resolved against it; a
hostless result is the page's own path (the lookup keys it by the page host)
unless the base names another host, when it gets that host and the document's
scheme, as the browser fetches it (`<base href="https://cdn.example.com/x/">`
with `href="/css/a.css"` is `https://cdn.example.com/css/a.css`; `<base
href="/sub/">` with `href="a.css"` is `/sub/a.css` on the page host). A
protocol-relative href gets the document's scheme whether or not the page has
a `<base>`: `href="//cdn.example.com/c.css"` is looked up as the CDN host's
`/c.css` (and `//example.com/d.css` as the page's own `/d.css`), where before
that the literal `//host/path` was looked up on the page host, a key nginx
never stores, so the sheet always counted as missing and the page was marked
for revalidation on every request; a `data:` or `javascript:` href, or any
other reference with a scheme, is returned as it is. Two edge
cases are documented, not handled: a `<link>` parsed BEFORE the `<base href>`
(a base after the first link, or inside `<body>`) is resolved against the base
here and by the scanner's identities, while the browser fetched it from the
page because the base did not exist yet — serve and analysis still agree, so
only that non-conforming page's sheet is looked up on the wrong host or its
twin pairing is wrong; and a same-host base with an explicit default port
(`<base href="https://example.com:443/">`) is the page's own host for fetching
(the default port is dropped for that comparison) while `SheetIdentity`
compares the port literally, so under it `/a.css` and
`https://example.com/a.css` are different identities (the same class as the
literal default-port comparison in the hero-copy test). Only `:443` on https and `:80` on
http are dropped for that comparison: a page served with a non-default port in
its Host header (`example.com:8443`) and a `<base href="https://example.com/">`
without the port counts as cross-host, which is what the browser does too, so
the hostless hrefs are fetched from `example.com` without the port.
Guards: 50 stylesheet cap, 2MB per-stylesheet cap, 10MB total HTML cap.
Stats: `css_inlining_attempted`, `css_inlining_stylesheets_cached`,
`css_inlining_bytes_inlined` (in BROWSER-STATUS, STATS, METRICS).

### Stats (STATS JSON / METRICS Prometheus)

Browser stats in `browser` object: `profiles_generated`, `profiles_used`,
`analysis_errors`, `chrome_crashes`, `queue_depth`, `scripts_analyzed`,
`scripts_deferrable`, `scripts_no_coverage` (evidence-free verdicts — scripts
the analysis browser could not observe), `script_map_scripts_found`,
`script_map_scripts_cached`, `script_map_scripts_uncached_same_origin`
(healable cache-cold misses; the cold-script TTL-heal trigger),
`script_map_scripts_empty_same_origin` (cached but zero-length; unhealable,
excluded from the heal trigger),
`script_map_scripts_cross_origin`, `script_map_scripts_oversize` (cached but
over the per-entry/total map caps; unhealable, excluded from the heal
trigger), `script_map_bytes_mapped`, `script_fetches_served`,
`script_fetches_blocked` (Fetch-interception outcomes in the coverage pass),
`css_inlining_attempted`, `css_inlining_stylesheets_cached`,
`css_inlining_bytes_inlined`. Full detail via `BROWSER-STATUS` management command.

## Optimization Policy Engine

Per-URL optimization policy computed from browser analysis profiles. The policy
engine decides whether to enable optional transforms (async CSS, script deferral)
based on measured page characteristics.

### Policy Fields

| Field | Type | Description |
|-------|------|-------------|
| `async_css_enabled` | bool | Enable async CSS loading for this URL |
| `script_deferral_enabled` | bool | Enable script deferral for this URL |

### Integration Flow

1. Browser analysis generates an `OptimizationProfile` (CSS coverage, script analysis)
2. `ComputePolicy()` evaluates profile data against thresholds
3. Policy result is stored with the profile in cache
4. HTML processing reads the policy and conditionally enables transforms

### Stats Counters

| Counter | Prometheus metric | Description |
|---------|-------------------|-------------|
| `policy.computed` | `pagespeed_policy_computed_total` | Optimization policies computed |
| `policy.async_css_enabled` | `pagespeed_policy_async_css_enabled_total` | Times async CSS was enabled by policy |
| `policy.async_css_suppressed_low_coverage` | `pagespeed_async_css_suppressed_low_coverage_total` | Times async CSS deferral was suppressed by the FOUC sufficiency gate (critical CSS too thin) |
| `policy.async_css_suppressed_unvalidated` | `pagespeed_async_css_suppressed_unvalidated_total` | Times async CSS deferral was suppressed because the page has no validation record bound to the stylesheet being served (not analyzed yet, or the stylesheet changed since) |
| `policy.async_css_record_dropped_empty_derivation` | `pagespeed_async_css_record_dropped_empty_derivation_total` | Times a profile's validation record was dropped before the gate because the DOM-matched derivation produced no critical block for the page being served (issue #1216) |
| `policy.script_deferral_enabled` | `pagespeed_policy_script_deferral_enabled_total` | Times script deferral was enabled by policy |

**Counter semantics (Issue E — these were dead/zero on prod before being wired
at the real decision site, not at the unused `OptimizationPolicy::Compute`).**
`policy.computed` increments once per HTML notification that reaches the
transform pass (i.e. past the no-transformation early-return) — it counts "a
policy decision was made", independent of whether the pass nets a change at the
later `modified()` check. `policy.async_css_enabled` / `policy.script_deferral_enabled`
increment when the corresponding feature (`has_async_css` /
`has_script_deferral`) is selected for that pass. The gate itself remains the
`transform_config` flags (e.g. async CSS = `!disable_async_css && has_critical_css`);
`OptimizationPolicy::Compute` stays telemetry-only and is NOT revived.

### Write-outcome counters (Issue E)

| Counter | Prometheus metric | Description |
|---------|-------------------|-------------|
| `alternates.writes` | `pagespeed_alternate_writes_total` | Alternate write attempts |
| `alternates.write_failures` | `pagespeed_alternate_write_failures_total` | HARD failures only (mmap/cache errors) |
| `alternates.writes_fenced` | `pagespeed_alternate_writes_fenced_total` | Benign writes dropped by a post-dispatch purge fence |
| `origin_refresh.purges` | `pagespeed_origin_refresh_purges_total` | Origin-refresh sentinels that purged a stale variant set |
| `origin_refresh.rate_limited` | `pagespeed_origin_refresh_rate_limited_total` | Origin-refresh sentinels rate-limited away |
| `origin_refresh.rechecked` | `pagespeed_notifications_origin_rechecked_total` | Dedup-hit notifications whose re-recorded original differed in origin state from the variant set |
| `origin_refresh.change_rebuilt` | `pagespeed_origin_change_rebuilt_total` | Variant sets purged (original kept) and rebuilt because the recorded bytes changed |
| `origin_refresh.unchanged_restamped` | `pagespeed_origin_unchanged_restamped_total` | Re-recorded originals with identical bytes: copies restamped, not rebuilt |

`WriteVariant` returns false for both benign purge-fences and hard cache errors.
The failure branches now probe `purge_check()` first: a post-dispatch purge
(e.g. the content-hash invalidation purge earlier in the same notification)
counts as `alternates.writes_fenced` and does NOT touch `errors`. Only genuine
cache/mmap failures count as `alternates.write_failures` + `errors`. This ends
the prior `write_failures == content_hash_stale == errors` dashboard lockstep.

### Module-stored HTML counters

A web-server module that stores HTML pages itself (C API 1.13 sited writes,
1.14 store params) notifies the worker with `FROM_MODULE | HOST_VOUCHED |
HTML_STORED`. `HandleModuleHtmlNotification` derives the Markdown copy from
the stored bytes, with no browser and no fetch, and only in a `markdown-*`
`html_mode` with `--agent-optimize` on. These counters are in `/v1/stats`
JSON (`STATS`) only; none has a Prometheus metric.

| Counter | Description |
|---------|-------------|
| `notifications.rejected_module_render` | Module frames refused: HTML in the event loop (not the stored-page flag set, a sentinel mask, `html_mode` `off` or `full`, `agent_optimize` off), and on the thread pool before any byte is copied (no stored page, not `kHtml` with a `text/html` type, no `PS_FLAG_HEADERS_CLASSIFIED`, `no-transform`, no content hash). Also non-HTML module frames that carry an agent request, the vouched bit or the llms.txt mask. A module frame with no site is counted in `notifications.rejected_empty_site` instead, before any of these checks |
| `agent_markdown.module_derived` | Markdown copies written from module-stored pages, bound to the page's `0x0C` stamp and stamped with their own content hash (the entity tag input) |
| `agent_markdown.module_withheld` | Permanent refusals recorded as a withheld marker (`0xCC`): `below_floor`, `too_large`, `encoded`, `hash_mismatch`, `extract_failed` |
| `agent_markdown.module_withheld_skips` | Frames that found a current marker for the content and did no derivation |
| `agent_markdown.below_floor` | Copies not kept because the body after front matter has fewer than 200 non-whitespace bytes, on both the module path and the browser render path |

The library refuses the frame shapes a module must never send
(`PS_ERR_INVALID_ARG` from `ps_notify_worker_ex`: HTML or an agent request
from the module without both `HOST_VOUCHED` and `HTML_STORED`), so those never
reach the worker or its counters. `tools/http-compliance/test_daemon_html.py`
drives this path end to end through `libpagespeed.so` with a module stand-in.

## SVG Auto-Vectorization

Automatic raster-to-SVG detection and conversion for logos, icons, and flat
illustrations. Three operational modes controlled by `--svg-mode`:

| Mode | Behavior |
|----------|------------------------------------------------------|
| `detect` | Evaluate candidacy, log scores. No vectorization. **(default)** |
| `preview` | Evaluate + vectorize + store SVG. Not served to clients. |
| `auto` | Full pipeline: evaluate, vectorize, store, serve. |

### Pipeline

```
Decode -> AnalyzeContent -> EvaluateSvgCandidacy -> PreprocessPixels
       -> VectorizeImage (VTracer FFI) -> SanitizeSvg -> PathCountGate
       -> SizeGate -> CacheWrite
```

SVG evaluation and vectorization are hoisted **before** the proactive raster
`(format, viewport, density, save-data)` loop. The image is decoded once at
original resolution, analyzed once, and vectorized once.

### Cache Behavior

SVG is resolution-independent. A single SVG variant serves all viewport classes,
pixel densities, and save-data states. Stored at the `kSvg` format slot
(`ImageFormat::kSvg = 3`, Desktop viewport). Pre-compressed gzip and brotli
variants are written alongside via `WriteCompressedVariants()`.

### LCP Exclusion

`--svg-exclude-lcp true` (default) skips SVG vectorization for images identified
as the LCP candidate by HtmlScanner or browser analysis. SVG render cost (path
tessellation) can regress LCP timing. Override with `--svg-exclude-lcp false`.

### SVG CLI Flags

```
--svg-mode MODE                 detect, preview, or auto (default: detect)
--svg-candidacy-threshold N     Score threshold (0-100, default 50)
--svg-max-pixels N              Max decoded pixels (default 65536)
--svg-max-paths N               Max <path> elements (default 500)
--svg-fidelity-threshold F      SSIMULACRA2 minimum (default 55.0)
--svg-exclude-lcp BOOL          Skip LCP images (default true)
--svg-timeout-ms N              Vectorization timeout (default 500)
--svg-preset N                  0=bw, 1=poster, 2=photo (default 1)
--svg-color-precision N         0=adaptive, 1-8=fixed (default 0)
--svg-filter-speckle N          Min cluster area (default 4)
```

All settings are hot-reloadable via `PATCH /v1/config`.

### SVG Stats (STATS JSON / METRICS Prometheus)

| Counter | Description |
|---|---|
| `svg_candidates_evaluated` | Images evaluated for SVG candidacy |
| `svg_candidates_rejected` | Rejected by candidacy score |
| `svg_vectorized` | Successfully vectorized |
| `svg_fidelity_rejected` | Vectorized but failed SSIMULACRA2 fidelity check |
| `svg_timeout_exceeded` | Vectorized but discarded for exceeding timeout |
| `svg_size_rejected` | Passed fidelity but raster was smaller |
| `svg_path_count_rejected` | Exceeded max path count |
| `svg_written` | SVG variants written to cache |
| `svg_bytes_saved` | Cumulative bytes saved (raster - SVG) |
| `svg_vectorize_time_us` | Vectorization latency histogram |
| `svg_served` | SVG variants served to clients (serve-time HITs; sourced from the cross-process ServeStats mmap `svg_optimized_hits`, written by both front-ends — follow-up #455, not a WorkerStats counter) |

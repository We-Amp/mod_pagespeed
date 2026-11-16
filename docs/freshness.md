# Cache Freshness: Conditional Revalidation & Origin CC Guidance

**Status:** Implemented
**Baseline:** designed against AlternateMetadata v5; the current format is v9 (`kCurrentVersion` in `lib/classify/alternate_metadata.h`)
**RFC references:** RFC 9111 (HTTP Caching), RFC 9110 (HTTP Semantics)

---

## 1. Problem Statement

mod_pagespeed acts as a shared cache. When origin content changes (deploy, CMS
publish, config update), the cache may serve stale content until `max-age`
expires. Behavior on stale content before this design:

| Scenario | Behavior before | Impact |
|----------|-----------------|--------|
| Stale + `must-revalidate` | Full re-fetch from origin | Correct but wasteful |
| Stale + no `must-revalidate` | Serve stale (with SWR) | May be acceptable |
| Origin sends no `Cache-Control` | Type defaults: HTML=0s, CSS=300s, Image=3600s | Surprise staleness |
| Deploy with hashed assets | Old HTML serves old URLs | Stale experience |

Two gaps:
1. **No conditional revalidation.** When stale content needs revalidation,
   PageSpeed makes a full request to origin. If origin returns the same content
   (common after infrastructure restarts, CDN flushes, or when only _other_ URLs
   changed), the full body is transferred, re-cached, and re-optimized for
   nothing. RFC 9111 Section 4.3 recommends conditional requests.
2. **No operator guidance.** Operators don't know what Cache-Control headers to
   set on their origins for optimal interaction with PageSpeed. Poor defaults
   lead to either permanent staleness (long max-age, no revalidation) or zero
   caching benefit (no-cache on everything).

---

## 2. Feature A: Conditional Revalidation (If-None-Match)

### 2.1 Design Overview

Store the origin's `ETag` and `Last-Modified` response headers in cache
metadata. When content is stale and requires revalidation, inject
`If-None-Match` / `If-Modified-Since` into the upstream request. On 304, update
the cache timestamp and serve from cache. On 200, record normally.

### 2.2 Metadata v5 Wire Format

Extend AlternateMetadata with two new fields appended after v4's content_class:

```
v4 suffix (17 bytes, fixed):
  [4B cache_inserted_at][4B origin_max_age][4B origin_s_maxage]
  [2B origin_cc_flags][2B ssimulacra2_score_x100][1B content_class]

v5 suffix (22 + etag_len bytes, variable):
  [4B cache_inserted_at][4B origin_max_age][4B origin_s_maxage]
  [2B origin_cc_flags][2B ssimulacra2_score_x100][1B content_class]
  [4B origin_last_modified][2B etag_len LE][etag_len bytes of etag...]
```

- `origin_last_modified`: uint32_t Unix timestamp. 0 = absent. (Note: 0
  collides with Unix epoch, but origins never send `Last-Modified: epoch`.
  Matches the same uint32_t limitation as `cache_inserted_at`.)
- `etag_len`: **uint16_t** LE (0-65535). 0 = no ETag stored. Using uint16_t
  instead of uint8_t costs 1 extra byte but eliminates the 255-byte cap
  entirely. In practice, ETags longer than ~100 bytes are rare.
- `etag`: raw ETag value from origin (e.g., `"abc123"` or `W/"abc123"`),
  stored verbatim including quotes and `W/` prefix if weak. Both weak and
  strong ETags are valid for `If-None-Match` revalidation per RFC 9110
  Section 13.1.2 (weak comparison).

The v5 suffix is variable-length: **23 fixed bytes** (v4's 17 + 4 for
`origin_last_modified` + 2 for `etag_len`) plus `etag_len` bytes of variable
etag data. Total wire size: `kFixedPrefixSize(9) + ct_len + 23 + etag_len`.

**Parsing logic:** `Deserialize()` reads the version byte first. For v5, after
reading the 17-byte v4 fields, it reads 4 bytes for `origin_last_modified`,
then 2 bytes for `etag_len`. It validates `etag_len` against remaining data
length. If the data is truncated (corruption), return `std::nullopt`.

**Backward compatibility:** Deserialize accepts v3, v4, or v5. Unknown versions
rejected. v3/v4 data gets default values (last_modified=0, etag_len=0, etag="").

**Version policy:** Backward compat is maintained across versions (no cache
flush required on upgrade). Old entries degrade gracefully — v3/v4 entries
simply skip conditional revalidation and use full re-fetch. This avoids MISS
storms on upgrade. The compliance doc's "cache flush on upgrade" guidance is
superseded; backward compat is the operational requirement going forward.

### 2.3 Recording Path Changes (MISS)

In `ngx_http_pagespeed_record_response()`, after parsing Cache-Control:

```cpp
// Extract ETag (stored verbatim, including quotes and W/ prefix).
if (r->headers_out.etag) {
    std::string_view etag_sv(
        reinterpret_cast<const char*>(r->headers_out.etag->value.data),
        r->headers_out.etag->value.len);
    if (etag_sv.size() <= 65535) {
        meta.origin_etag = std::string(etag_sv);
    }
    // ETags > 65535 bytes silently skipped (conditional revalidation
    // disabled for this URL). Increment stats counter.
}

// Extract Last-Modified (as Unix timestamp).
if (r->headers_out.last_modified_time > 0) {
    meta.origin_last_modified =
        static_cast<uint32_t>(r->headers_out.last_modified_time);
}
```

The `origin_etag` and `origin_last_modified` fields are also recorded in the
200-during-revalidation path (when origin returns 200 after a conditional
request). This ensures new ETag/Last-Modified values are captured for the
next revalidation cycle.

### 2.4 Serving Path Changes (HIT — stale revalidation)

The revalidation trigger expands to include `no-cache`:

```cpp
// no-cache means "revalidate before each use" (RFC 9111 Section 5.2.2.4).
// Effectively max-age=0 + must-revalidate.
bool is_no_cache = (cc_flags & AlternateMetadata::kCCOriginNoCache);

// Single shared helper (lib/cache/cache_control_header.h) — gates
// proxy-revalidate and s-maxage on is_shared_cache per RFC 9111 §5.2.2.9–10.
bool revalidation_required =
    ComputeSharedRevalidationRequired(cc_flags, input.is_shared_cache);

// For no-cache: always stale (every request must revalidate).
bool is_stale = is_no_cache || (age >= effective_max_age);

if (revalidation_required && is_stale) {
    if (conf->conditional_revalidation &&
        (meta.origin_etag.size() > 0 || meta.origin_last_modified > 0)) {
        // Conditional revalidation path.
        ctx->stale_revalidation = 1;
        ctx->stale_read_result = ctx->read_result;
        ctx->read_result = nullptr;
        ctx->served_from_cache = 0;
        inject_conditional_headers(r, ctx, meta);
        goto cache_miss;
    }
    // No revalidation tokens or disabled: full re-fetch.
    delete ctx->read_result;
    ctx->read_result = nullptr;
    ctx->served_from_cache = 0;
    goto cache_miss;
}
```

### 2.5 Conditional Header Injection

`inject_conditional_headers()` saves the client's original conditional headers
and injects stored values for the upstream request:

```cpp
static void inject_conditional_headers(ngx_http_request_t* r,
                                       ngx_http_pagespeed_ctx_t* ctx,
                                       const AlternateMetadata& meta) {
    // Save client's original conditional headers for later restoration.
    // The client's If-None-Match contains PageSpeed's weak ETag (W/"ps-..."),
    // which is in a completely different namespace from the origin's ETag.
    ctx->saved_client_if_none_match = r->headers_in.if_none_match;
    ctx->saved_client_if_modified_since = r->headers_in.if_modified_since;

    // Inject If-None-Match with stored origin ETag.
    if (!meta.origin_etag.empty()) {
        ngx_table_elt_t* h = ngx_list_push(&r->headers_in.headers);
        // ... set h->key = "If-None-Match", h->value = meta.origin_etag ...
        h->hash = 1;
        // CRITICAL: set the shortcut pointer. Without this, some nginx
        // code paths (proxy module, not_modified filter) won't see the
        // injected header.
        r->headers_in.if_none_match = h;
    }

    // Inject If-Modified-Since (only when no ETag available — per RFC 9110
    // Section 13.2.2, servers MUST ignore IMS when INM is present, but
    // sending both is a compatibility measure for origins that only
    // implement IMS).
    if (meta.origin_last_modified > 0) {
        ngx_table_elt_t* h = ngx_list_push(&r->headers_in.headers);
        // ... format origin_last_modified as HTTP-date ...
        h->hash = 1;
        r->headers_in.if_modified_since = h;
        // Note: ngx_http_headers_in_t has no parsed time field;
        // the proxy module reads the header value string directly.
    }
}
```

### 2.6 Header Filter Changes (304 Handling)

In `ngx_http_pagespeed_header_filter()`. On 304, the header filter prepares
the response headers and sets a flag for the body filter to deliver cached
content. **It does NOT call `ngx_http_output_filter()` directly** — sending
the body from the header filter is unsafe (reentrancy, stack-allocated chain
use-after-free on NGX_AGAIN, and upstream state machine corruption).

```cpp
if (ctx && ctx->stale_revalidation &&
    r->headers_out.status == NGX_HTTP_NOT_MODIFIED) {

    // ---- Update stored metadata per RFC 9111 Section 4.3.4 ----
    auto* stale = ctx->stale_read_result;
    AlternateMetadata updated_meta = stale->metadata;

    // Reset freshness. Adjust by inbound Age header from the 304
    // response (same logic as initial recording per RFC 9111 D3).
    updated_meta.cache_inserted_at = static_cast<uint32_t>(ngx_time());
    ngx_str_t inbound_age_304 =
        ngx_http_pagespeed_get_response_header(r, "Age");
    if (inbound_age_304.len > 0) {
        std::string_view age_sv(
            reinterpret_cast<const char*>(inbound_age_304.data),
            inbound_age_304.len);
        // Age is delta-seconds, the same grammar and the same saturating
        // parse as a Cache-Control lifetime, so it is the shared one.
        uint32_t age_val = pagespeed::ParseCacheControlSeconds(age_sv);
        if (age_val > 0 && age_val <= updated_meta.cache_inserted_at) {
            updated_meta.cache_inserted_at -= age_val;
        }
    }

    // Update Cache-Control if 304 carried new directives. The derivation
    // itself is pagespeed::AccumulateOriginCacheControl in lib/cache; the
    // module's helper is the header walk around it.
    auto new_cc = ngx_http_pagespeed_parse_cache_control(r);
    if (new_cc.cc_flags & AlternateMetadata::kCCOriginHeaderPresent) {
        updated_meta.origin_max_age = new_cc.max_age;
        updated_meta.origin_s_maxage = new_cc.s_maxage;
        updated_meta.origin_cc_flags = new_cc.cc_flags;
    }

    // Update ETag if 304 carried a new one (RFC 9111 Section 4.3.4).
    // Store verbatim including W/ prefix.
    if (r->headers_out.etag) {
        std::string_view new_etag(
            reinterpret_cast<const char*>(r->headers_out.etag->value.data),
            r->headers_out.etag->value.len);
        updated_meta.origin_etag = std::string(new_etag);
    }

    // Update Last-Modified if 304 carried a new one.
    if (r->headers_out.last_modified_time > 0) {
        updated_meta.origin_last_modified =
            static_cast<uint32_t>(r->headers_out.last_modified_time);
    }

    // Check Vary consistency (RFC 9111 Section 4.3). If the 304
    // includes Vary dimensions not handled by the capability mask,
    // treat as uncacheable — fall through to full re-fetch.
    if (ngx_http_pagespeed_vary_uncacheable_from_304(r)) {
        delete ctx->stale_read_result;
        ctx->stale_read_result = nullptr;
        ctx->stale_revalidation = 0;
        // Let upstream 304 pass through (client will see 304 or error).
        return ngx_http_next_header_filter(r);
    }

    // ---- Write updated alternate to cache ----
    // Copies content from the mmap'd stale entry. Write-new-alternate
    // naturally updates the RAM cache and is atomic.
    auto content = stale->content();
    // Copy to temp buffer before writing — the mmap source is in the
    // same Cyclone volume, and writes may trigger internal remapping.
    std::string content_copy(
        reinterpret_cast<const char*>(content.data()), content.size());
    auto wh = cache->WriteAlternate(url, hostname, id,
                                    content_copy.size(), updated_meta);
    wh->write_sync(as_bytes(content_copy));
    wh->close_sync();

    // Evict old RAM cache entry so next request sees fresh metadata.
    cache->EvictRamCache(url, hostname, id);

    // ---- Prepare to serve cached content ----
    // Restore client's original conditional headers BEFORE passing
    // to ngx_http_next_header_filter. This prevents the
    // not_modified_filter from comparing the injected origin ETag
    // against PageSpeed's weak ETag (different namespaces).
    r->headers_in.if_none_match = ctx->saved_client_if_none_match;
    r->headers_in.if_modified_since = ctx->saved_client_if_modified_since;

    // Rewrite response status and headers.
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = content_copy.size();
    // ... set Content-Type, Cache-Control, ETag, Age, Vary ...
    // ... same header logic as the normal HIT path ...

    // Add debugging header.
    // (X-PageSpeed header set to "REVALIDATED" instead of "HIT")
    add_response_header(r, "X-PageSpeed", "REVALIDATED");
    add_response_header(r, "X-PageSpeed-Revalidation", "304");

    // Transfer ownership: body filter will serve the cached content.
    ctx->read_result = stale;
    ctx->stale_read_result = nullptr;
    ctx->stale_revalidation = 0;
    ctx->served_from_cache = 1;
    ctx->serve_stale_body = 1;  // Signal body filter
    ctx->done = 1;              // Prevent body recording

    // Increment stats.
    ngx_atomic_fetch_add(&g_conditional_304s, 1);

    return ngx_http_next_header_filter(r);
}

// Handle 200 during stale revalidation: content changed.
if (ctx && ctx->stale_revalidation &&
    r->headers_out.status >= 200 && r->headers_out.status < 300) {
    // Free stale entry — content has changed.
    delete ctx->stale_read_result;
    ctx->stale_read_result = nullptr;
    ctx->stale_revalidation = 0;
    // Proceed with normal MISS recording (existing code path).
    // The body filter will buffer the new response and record it.
    // New ETag/Last-Modified from the 200 will be captured in
    // record_response() (Section 2.3).
    ngx_atomic_fetch_add(&g_conditional_200s, 1);
}
```

### 2.7 Body Filter Changes

The body filter handles two stale-revalidation scenarios:

```cpp
// Case 1: 304 — serve cached content instead of (empty) upstream body.
if (ctx && ctx->serve_stale_body) {
    ctx->serve_stale_body = 0;
    auto cached_data = ctx->read_result->content();

    // Pool-allocate the chain (never stack-allocate ngx_chain_t —
    // if ngx_http_next_body_filter returns NGX_AGAIN, nginx's write
    // event handler will reference the chain later).
    ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
    b->pos = const_cast<u_char*>(
        reinterpret_cast<const u_char*>(cached_data.data()));
    b->last = b->pos + cached_data.size();
    b->memory = 1;
    b->last_buf = 1;
    ngx_chain_t* out = static_cast<ngx_chain_t*>(
        ngx_palloc(r->pool, sizeof(ngx_chain_t)));
    out->buf = b;
    out->next = nullptr;

    return ngx_http_next_body_filter(r, out);
}

// Case 2: 200 during stale revalidation — normal recording proceeds.
// (No special handling needed; stale_revalidation was cleared in
// header filter, body filter records normally.)
```

### 2.8 Request Context Extension

```cpp
typedef struct {
    CapabilityMask* mask;
    ngx_chain_t* buffered_body;
    ngx_chain_t** buffered_last;
    ReadResult* read_result;
    ReadResult* stale_read_result;               // Stale entry during revalidation
    ngx_table_elt_t* saved_client_if_none_match;  // Client's original INM
    ngx_table_elt_t* saved_client_if_modified_since; // Client's original IMS
    size_t recorded_bytes;
    unsigned recording : 1;
    unsigned done : 1;
    unsigned waiting_for_worker : 1;
    unsigned served_from_cache : 1;
    unsigned uncacheable : 1;
    unsigned no_transform : 1;
    unsigned head_request : 1;
    unsigned stale_revalidation : 1;             // Conditional revalidation in progress
    unsigned serve_stale_body : 1;               // Body filter: serve cached content
} ngx_http_pagespeed_ctx_t;
```

### 2.9 Cleanup Handler

```cpp
static void ngx_http_pagespeed_cleanup(void* data) {
    auto* ctx = static_cast<ngx_http_pagespeed_ctx_t*>(data);
    delete ctx->mask;
    ctx->mask = nullptr;
    delete ctx->read_result;
    ctx->read_result = nullptr;
    // CRITICAL: clean up stale_read_result on ALL exit paths —
    // upstream timeout, 5xx, client disconnect, nginx shutdown.
    delete ctx->stale_read_result;
    ctx->stale_read_result = nullptr;
}
```

### 2.10 Worker Re-notification

On 304: no worker notification needed. The optimized variants are still valid
because the origin content has not changed. This saves 30-120 seconds of CPU
per image URL (AVIF encodes) and 3-6 cache writes per revalidated URL.

On 200 (content actually changed): existing recording + notification path runs
unchanged. `stale_read_result` is freed in header filter. New content is
recorded with new ETag/Last-Modified. Worker re-optimizes all variants.

### 2.11 Stats

New per-nginx-worker atomic counters:

- `conditional_revalidations`: Conditional requests sent to origin.
- `conditional_304s`: 304 responses received (cache reused).
- `conditional_200s`: 200 responses received (content changed).
- `etag_too_long`: ETags exceeding 65535 bytes (silently skipped).

**Invariant:** `conditional_revalidations == conditional_304s + conditional_200s
+ upstream_errors_during_revalidation`.

**Exposure mechanism:** These counters live in the nginx module process, not
the worker. Two options:

1. **Shared memory zone** (`ngx_shared_memory_add`): Allocate a small shared
   memory zone (`pagespeed_stats`) for atomic counters. Both the worker
   (via management socket) and nginx workers can read/increment. Worker's
   STATS/METRICS endpoints aggregate from shared memory.
2. **Nginx stub_status-style endpoint**: Expose counters via a new nginx
   location handler (`pagespeed_status`). Simpler but separate from worker
   stats.

Prefer option 1 for unified stats. Option 2 as fallback if shared memory
adds too much complexity.

### 2.12 Per-Request Debugging

Operators diagnosing stale content issues need per-request signals visible
via `curl -v`:

| Header | Value | Meaning |
|--------|-------|---------|
| `X-PageSpeed` | `HIT` | Served from cache (fresh) |
| `X-PageSpeed` | `REVALIDATED` | Served from cache after 304 from origin |
| `X-PageSpeed` | `MISS` | Fetched from origin (cache miss or content changed) |
| `X-PageSpeed-Revalidation` | `304` | Origin confirmed content unchanged |
| `X-PageSpeed-Revalidation` | `200` | Origin returned new content |

`X-PageSpeed-Revalidation` is only present when conditional revalidation was
attempted. Absent on normal HIT or MISS.

### 2.13 Configuration

```
pagespeed_conditional_revalidation on|off;  # default: on
```

Provides an explicit escape hatch if conditional revalidation causes issues
with a specific origin (e.g., origin sends ETags but doesn't handle
`If-None-Match` correctly). Disabling falls back to full re-fetch on stale
content (existing behavior). Does not disable caching itself.

---

## 3. Feature B: Origin Cache-Control Guidance & Improved Defaults

**IMPORTANT:** Phase 3 (operator guidance) MUST NOT ship before Phase 2
(conditional revalidation). Without conditional revalidation, recommending
`max-age=0, must-revalidate` causes every HTML request to bypass the cache
entirely — a performance regression disguised as best-practice guidance. The
documentation must carry a version prerequisite.

### 3.1 Configurable Per-Type Defaults

Currently hardcoded (lines 1281-1291 of `ngx_pagespeed_module.cc`):
```
HTML with no CC header → 0 (no-cache)
CSS/JS with no CC header → 300s
Images with no CC header → 3600s
```

Add three new nginx directives (naming follows existing `pagespeed_max_age`
pattern, without "default" prefix for consistency):
```
pagespeed_html_max_age N;    # default: 0
pagespeed_css_max_age N;     # default: 300
pagespeed_image_max_age N;   # default: 3600
```

These only apply when the origin sends NO Cache-Control and NO Expires header.
When origin sends headers, they always take precedence.

### 3.2 Missing Cache-Control Warning

When origin sends no Cache-Control header (bit 9 of origin_cc_flags not set),
log a warning on first occurrence per hostname + content type:

```
pagespeed: origin for example.com sent no Cache-Control header for text/html
  response. Using default max-age=0. Set Cache-Control headers on your origin
  for optimal caching behavior. See https://modpagespeed.com/docs/cache-control
```

Rate-limited: one warning per hostname + content-type combination per nginx
worker lifetime (tracked in a hash set, capped at 1024 entries to bound
memory at ~64KB regardless of tenant count; when full, stop logging new
warnings).

### 3.3 Recommended Origin Headers

Documentation page at `/docs/cache-control/` covering:

> **Prerequisite:** These recommendations require PageSpeed 2.x+ with
> conditional revalidation support. Without it, `must-revalidate` causes
> full origin re-fetches on every stale request.

**HTML pages:**
```
Cache-Control: public, max-age=60, must-revalidate
ETag: "content-hash-or-version"
```
Why: HTML references hashed assets. A deploy changes HTML content to point to
new asset URLs. Short `max-age=60` provides 1-minute caching. With conditional
revalidation, stale requests are cheap (304 when unchanged). Use `max-age=0`
only when absolute real-time freshness is required (e.g., stock tickers).

**Hashed static assets (CSS/JS with fingerprints):**
```
Cache-Control: public, max-age=31536000, immutable
```
Why: URL changes on every build. Content at a given URL never changes.
PageSpeed respects `immutable` and caps at `pagespeed_immutable_max_age`.

**Non-hashed static assets:**
```
Cache-Control: public, max-age=3600, must-revalidate
ETag: "file-mtime-and-size"
```
Why: URL is stable, content may change. Short max-age + revalidation keeps
content fresh. Conditional revalidation makes the revalidation cheap.

**User-uploaded images:**
```
Cache-Control: public, max-age=86400
```
Why: User images rarely change at the same URL. 24h caching is reasonable.
If images can be replaced: add `must-revalidate` and an ETag.

**API responses / dynamic content:**
```
Cache-Control: no-store
```
Why: Not cached by PageSpeed at all (header filter marks as uncacheable).

### 3.4 Framework-Specific Examples

Quick-reference configurations for common frameworks. These are concise
pointers; the full `/docs/cache-control/` page will include copy-pasteable
config snippets for each.

| Framework | Config Location | HTML Headers | Asset Headers |
|-----------|----------------|--------------|---------------|
| Astro (4+) | `astro.config.mjs` server adapter `headers` | `max-age=60, must-revalidate` | `immutable` (automatic via content hashing) |
| Next.js (13+ App Router) | `next.config.js` `headers()` or route `export const revalidate` | `max-age=60, must-revalidate` | `immutable` (automatic for `_next/static/`) |
| nginx (static files) | `nginx.conf` `add_header` per location | `max-age=60, must-revalidate` | `max-age=31536000, immutable` |
| Apache | `.htaccess` `Header set` | `max-age=60, must-revalidate` | `max-age=31536000, immutable` |
| WordPress | WP Super Cache / W3 Total Cache plugin settings, or `.htaccess` | `max-age=60, must-revalidate` | Varies (theme-dependent) |
| Rails (7+) | `config/application.rb` `config.public_file_server.headers` | `max-age=60, must-revalidate` | `immutable` (via Propshaft content hashing) |

### 3.5 Console Integration

The web console already shows `Cache-Control` metadata per alternate. Add:
- Warning badge when origin sends no Cache-Control header
- Link to the `/docs/cache-control/` guide
- Per-URL freshness indicator:
  - "Fresh (Nm remaining)" — content within max-age
  - "Revalidated Nm ago (304)" — refreshed via conditional revalidation
  - "Stale (awaiting revalidation)" — past max-age
- Separate "inserted" vs "last revalidated" timestamps where the data allows
  distinguishing original recording from 304 refresh.

---

## 4. Implementation Plan

### Phase 1: Metadata v5 + Recording (1 commit)
1. Extend `AlternateMetadata` with `origin_etag` (string) and
   `origin_last_modified` (uint32_t)
2. Add `etag_len` as uint16_t to wire format
3. Update Serialize/Deserialize (accept v3/v4/v5)
4. Update `WireSize()` to account for variable etag length
5. Record ETag + Last-Modified in nginx recording path
6. Unit tests: v5 round-trip, v3/v4 backward compat, truncated data,
   long ETag, empty ETag, weak vs strong ETag preservation

### Phase 2: Conditional Revalidation (1 commit)
1. Add context fields: `stale_revalidation`, `stale_read_result`,
   `saved_client_if_none_match`, `saved_client_if_modified_since`,
   `serve_stale_body`
2. Update cleanup handler to free `stale_read_result`
3. Expand revalidation trigger to include `no-cache`
4. Implement `inject_conditional_headers()` with client header save/restore
   and shortcut pointer assignment
5. Handle 304 in header filter (prepare headers, set `serve_stale_body` flag)
6. Handle cached body delivery in body filter (pool-allocated chain)
7. Handle 200 in header filter (free stale, proceed with normal recording)
8. Add `pagespeed_conditional_revalidation on|off` directive
9. Shared memory stats counters
10. X-PageSpeed / X-PageSpeed-Revalidation response headers
11. E2E test: origin with ETag → verify 304 behavior, 200 on change,
    upstream timeout cleanup, client disconnect cleanup

### Phase 3: Operator Guidance (1 commit, ships AFTER Phase 2)
1. Add `pagespeed_html_max_age`, `pagespeed_css_max_age`,
   `pagespeed_image_max_age` directives
2. Add rate-limited missing-CC warning log (capped hash set)
3. Write `/docs/cache-control/` documentation page with version prerequisite
4. Console warning badge for missing Cache-Control

### Phase 4 (future): Background Revalidation
- SWR-aware: serve stale immediately, revalidate in background
- Per-URL revalidation lock (prevent thundering herd — N concurrent stale
  requests for the same URL sending N upstream requests)
- Proactive revalidation before expiry

### Phase 5 (future): stale-if-error
- Serve stale content when origin is unreachable (RFC 9111 Section 5.2.2.11)
- Requires storing stale entries for a configurable grace period after eviction

---

## 5. Edge Cases and Risks

### 5.1 Conditional Request Injection

nginx's `proxy_pass` forwards request headers to upstream. We inject
`If-None-Match` / `If-Modified-Since` into `r->headers_in`.

**Client collision:** The _client_ may send `If-None-Match` with PageSpeed's
weak ETag (`W/"ps-..."`) for its own cache validation. This is in a completely
different namespace from the origin's ETag. We must save the client's headers
before injection and restore them before `ngx_http_next_header_filter` runs
(mandatory — not optional mitigation). Without restoration, the
`not_modified_filter` would compare the injected origin ETag against
PageSpeed's response ETag and potentially send an incorrect 304 to the client.

**Shortcut pointers:** When injecting headers into `r->headers_in.headers`,
we MUST also set `r->headers_in.if_none_match` and
`r->headers_in.if_modified_since` (the shortcut pointers). Without these,
some nginx code paths (proxy module internals, not_modified filter) won't see
the injected headers, causing silent failures where conditional requests are
never sent.

**proxy_set_header override:** If the operator has
`proxy_set_header If-None-Match ""` in their config, the injected header will
be stripped. Document as known limitation.

### 5.2 304 Body Handling (header filter → body filter split)

A 304 response has no body. The cached content is delivered via the body filter
using the `serve_stale_body` flag, not from the header filter.

**Rationale:** Calling `ngx_http_output_filter()` from within the header filter
is unsafe for three reasons:
1. **Reentrancy:** Output filter calls the body filter chain, which includes
   our own body filter. This creates unexpected reentrancy.
2. **Stack-allocated chain:** If `ngx_http_output_filter` returns `NGX_AGAIN`
   (socket buffer full), nginx's write event handler will reference the
   chain later, but a stack-allocated `ngx_chain_t` would be destroyed.
3. **Upstream state machine:** The upstream module expects exclusive control
   of body delivery. Injecting body from the header filter corrupts its state.

The body filter approach follows nginx's expected header-then-body sequencing.

### 5.3 Origin Returns 200 During Stale Revalidation

Content has changed. In the header filter:
1. Free `stale_read_result` (delete + nullptr)
2. Clear `stale_revalidation` flag
3. Restore client's original conditional headers
4. Proceed with normal MISS handling (body filter buffers + records)
5. `record_response()` captures new ETag/Last-Modified from the 200 response
   into v5 metadata, ensuring the next revalidation cycle has fresh tokens

### 5.4 Upstream Error During Stale Revalidation

If origin returns 5xx, times out, or the connection fails:
- `stale_read_result` is cleaned up by `ngx_http_pagespeed_cleanup()` (pool
  cleanup handler) when the request is destroyed.
- No conditional revalidation stats are incremented (counts toward "upstream
  errors during revalidation" in the invariant).
- Client sees the upstream error (502/504) from proxy_pass, same as without
  conditional revalidation.

Future Phase 5 (stale-if-error) would serve the stale content in this case
instead of returning an error.

### 5.5 Write Amplification on 304

On 304, we write a full new alternate with the same content to update metadata.
For large content (200KB+ HTML), this is significant I/O for just changing
`cache_inserted_at`.

**Mitigations:**
- The alternative (current behavior) is a full re-fetch + full write + worker
  notification + all variant re-optimizations. The 304 path is still much
  cheaper.
- Use `max-age=60` (not `max-age=0`) for HTML guidance. At 60s freshness,
  revalidation occurs at most once per minute per URL, not once per request.
- Future optimization: in-memory-only timestamp update (skip disk write on 304,
  accept one extra conditional request after nginx restart when disk metadata
  is re-read as stale). Deferred to Phase 4.

**Estimate at 1000 RPS, 100 unique HTML pages, max-age=60:**
~100 writes/minute × 200KB = ~333KB/s disk writes. Acceptable.

### 5.6 304 Header Update Semantics (RFC 9111 Section 4.3.4)

On 304, we update stored metadata from the 304 response headers:
- **Updated:** `Cache-Control`, `ETag`, `Last-Modified`, `cache_inserted_at`
  (adjusted by inbound Age header)
- **NOT updated:** `Content-Type`, `Content-Length`, `Content-Encoding`
  (these describe the body, which hasn't changed)
- **Vary check:** If the 304 includes `Vary` headers with dimensions not
  covered by the capability mask, treat as uncacheable (skip revalidation,
  fall through to full re-fetch)

### 5.7 Worker-Written Variants

Conditional revalidation only applies to the origin alternate (AlternateId
0x08). Worker-written variants (optimized images, minified CSS, etc.) are
revalidated indirectly:
- On 304: worker variants remain valid (origin content unchanged)
- On 200: origin alternate is replaced, worker is re-notified, all variants
  re-optimized

Worker variants inherit `origin_etag` / `origin_last_modified` from the origin
metadata, but these fields are not used for worker variants (they have different
content). Conditional revalidation is only attempted when the stored
`AlternateId` is the origin default (0x08).

### 5.8 Clock Skew

`origin_last_modified` is stored as the origin's `Last-Modified` Unix timestamp,
not our local clock. `If-Modified-Since` sends the stored value back to origin
unchanged. No clock skew issue.

Use `ngx_time()` (nginx's cached time) instead of `time(nullptr)` (syscall)
for `cache_inserted_at` updates, consistent with nginx conventions and avoiding
per-request syscall overhead.

### 5.9 RAM Cache Staling After 304

After writing the updated alternate to disk, call `EvictRamCache()` so the
next request picks up fresh metadata from disk. The write-new-alternate
approach via `WriteAlternate()` naturally updates the RAM cache entry, but
an explicit eviction ensures correctness even under race conditions.

### 5.10 SWR Interaction with Conditional Revalidation

Conditional revalidation fires only when `revalidation_required && is_stale`.
For content without `must-revalidate`/`proxy-revalidate`/`s-maxage`/`no-cache`,
the SWR window allows serving stale content without revalidation. This is
intentional:

- **With revalidation directives:** Every stale request triggers conditional
  revalidation (correct per RFC 9111).
- **Without revalidation directives:** Stale content served with synthesized
  SWR. Downstream caches may revalidate, but PageSpeed does not.
- **Background revalidation during SWR:** Deferred to Phase 4. In Phase 4,
  the first request in the SWR window would trigger an async conditional
  revalidation while serving stale, so the next request gets fresh content.

### 5.11 Thundering Herd on Stale Content

When N concurrent requests hit a stale URL, each independently sends a
conditional request to origin. No request coalescing exists.

**Impact:** N upstream requests instead of 1. Each successful 304 writes a new
alternate (last writer wins, all content identical).

**Mitigation (Phase 2):** Document as known gap.
**Mitigation (Phase 4):** Per-URL revalidation lock. First request revalidates;
subsequent requests wait or serve stale (SWR-style).

### 5.12 PURGE Interaction

After `POST /v1/cache/purge`, the cache entry is deleted. The next request is
a full MISS with no stale entry to revalidate against. This is correct:
- PURGE = immediate invalidation, full re-fetch, worker re-optimization
- Natural expiration + conditional revalidation = gradual, bandwidth-efficient

Document this distinction in `/docs/cache-control/` so operators can choose
the appropriate freshness mechanism.

---

## 6. Alternatives Considered

### 6.1 Deploy Webhook (purge-on-deploy)
Simpler but requires operator action on every deploy. Conditional revalidation
is automatic and handles all cases (deploy, CDN flush, infrastructure restart).
The purge API remains useful as an explicit escape hatch but should not be the
primary freshness mechanism.

### 6.2 Content Hashing
Compare response body hash with cached body hash on every MISS. Detects content
changes without ETag support. But: requires full body transfer before comparison,
defeating the purpose of bandwidth savings. Conditional revalidation avoids
transferring the body entirely on 304.

### 6.3 Background Revalidation (Phase 4)
Proactively revalidate stale content before it's requested. Reduces latency for
the first request after staleness. Requires a background timer/thread or
SWR-triggered async revalidation. Better as Phase 4 after conditional
revalidation is proven.

### 6.4 stale-if-error (Phase 5)
Serve stale content when origin is unreachable. Orthogonal to this proposal but
complementary. Would require storing stale entries for a configurable grace
period.

### 6.5 In-Place Metadata Update (deferred optimization)
Instead of writing a full new alternate on 304, update only the metadata prefix
in-place. Eliminates write amplification but requires either:
(a) migrating metadata from content prefix to Cyclone's `set_header()` field
    (architectural change), or
(b) implementing partial content-prefix rewriting (complex, Cyclone doesn't
    support partial content updates)

Deferred as a Phase 4 optimization. The write-new-alternate approach is correct
and the write amplification is bounded by recommending `max-age=60` (not 0)
for HTML.

### 6.6 nginx Variables for Conditional Headers
Define nginx variables (`$pagespeed_if_none_match`) set during the access phase
and used via `proxy_set_header`. Avoids manipulating `r->headers_in` but
requires operators to add directives to their nginx.conf. Rejected in favor of
transparent injection (zero-config).

---

## 7. Expert Review Summary

This design was reviewed by a panel of four domain experts. Key findings
incorporated into v2:

| Finding | Source | Resolution |
|---------|--------|------------|
| Don't send body from header filter | nginx expert | Body delivered via body filter with `serve_stale_body` flag (Section 2.7) |
| Set `r->headers_in` shortcut pointers | nginx expert | Mandatory in `inject_conditional_headers` (Section 2.5) |
| `stale_read_result` cleanup in all error paths | nginx + perf | Added to cleanup handler (Section 2.9) |
| Pool-allocate `ngx_chain_t` | nginx expert | Pool allocation in body filter (Section 2.7) |
| Restore client conditional headers (mandatory) | nginx + HTTP | Mandatory before `ngx_http_next_header_filter` (Sections 2.5, 2.6) |
| RFC 9111 4.3.4 header update incomplete | HTTP expert | Update CC, ETag, Last-Modified, adjust Age (Section 2.6) |
| Inbound Age adjustment on 304 | HTTP expert | Same logic as initial recording (Section 2.6) |
| Vary consistency check on 304 | HTTP expert | Fall through to full re-fetch on Vary change (Section 2.6) |
| `no-cache` should trigger revalidation | HTTP expert | Added to revalidation trigger (Section 2.4) |
| ETag: use uint16_t | all four | Changed from uint8_t to uint16_t (Section 2.2) |
| Stats counter for truncated ETags | product expert | Added `etag_too_long` counter (Section 2.11) |
| Stats live in nginx, not worker | product expert | Shared memory or nginx endpoint (Section 2.11) |
| Per-request debugging headers | product expert | X-PageSpeed-Revalidation header (Section 2.12) |
| On/off directive | product expert | `pagespeed_conditional_revalidation` (Section 2.13) |
| Phase ordering: revalidation before guidance | product expert | Phase 3 requires Phase 2 (Section 3 header) |
| Write amplification on 304 | perf expert | Bounded by max-age=60 recommendation; deferred optimization (Section 5.5) |
| Thundering herd | perf expert | Documented as known gap; Phase 4 (Section 5.11) |
| Copy mmap'd content before writing | nginx expert | Added to 304 handler (Section 2.6) |
| Use `ngx_time()` not `time(nullptr)` | nginx expert | Updated (Section 5.8) |
| Directive naming consistency | product expert | `pagespeed_html_max_age` (no "default" prefix) (Section 3.1) |
| Remove `RewriteMetadata` API | HTTP + product | Removed; write-new-alternate only (Sections 2.6, 6.5) |
| Version policy: backward compat, no cache flush | HTTP expert | Documented as policy (Section 2.2) |
| Warning log hash set cap | perf expert | Capped at 1024 entries (Section 3.2) |

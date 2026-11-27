# Compression Flow Analysis

Status: **Phase 1 (P0 fix), Phase 2 (comments/tuning), and Phase 4 (bit space redesign + pre-compressed alternates) COMPLETE. Phase 3 (dynamic brotli) pending.**

Reviewed by: nginx/HTTP internals, cache/worker integrity, security/ops, performance.

## Architecture

Cache stores both uncompressed (identity) and pre-compressed (gzip, brotli)
alternates for text resources (HTML, CSS, JS). The worker produces all three
encodings after optimization. nginx serves the pre-compressed alternate directly
when available (zero dynamic compression cost), falling back to identity +
dynamic compression when no matching encoding exists. Images are stored as
identity only (already compressed formats).

## Current Flow

```
CLIENT (Accept-Encoding: gzip, br)
       │
  ┌────▼────┐
  │  nginx   │── HIT ──► Read from Cyclone (uncompressed)
  │  access  │                    │
  └────┬─────┘                    ▼
       │ MISS               gzip filter ──► CLIENT
  ┌────▼─────┐
  │ proxy to │  Accept-Encoding passed through (!)
  │  origin  │
  └────┬─────┘
  ┌────▼──────────┐
  │ PS body filter │  Records body bytes to cache
  │ (runs first)   │
  └────┬──────────┘
       ▼
  gzip filter ──► CLIENT

  Async: Worker reads from cache → optimizes → writes variants (uncompressed)
```

## What's Correct

1. **Body filter position**: Registers at top of filter chain
   (`ngx_pagespeed_module.cc:1250-1251`), runs before gzip filter, captures
   uncompressed bytes.

2. **Alternate selection includes encoding**: `PageSpeedSelector` scores by
   viewport (+2000), format (+1000), encoding (+60 exact, +5 identity fallback),
   density (+40), save-data (+20). Mismatched non-identity encoding is a hard
   disqualification (score 0). This enables pre-compressed cache alternates
   (Phase 4.4+) while ensuring identity fallback always works.

3. **Vary headers on HIT** (`ngx_pagespeed_module.cc:992-1015`):
   - HTML: `Vary: Accept-Encoding, User-Agent`
   - CSS/JS: `Vary: Accept-Encoding`
   - Images: `Vary: Accept, Save-Data, User-Agent`

4. **Cyclone's CompressionAwareSelector bypassed** (`alternate_id.h:16-20`):
   PageSpeed owns AlternateId semantics entirely.

5. **Minify-then-compress order is correct**: Minified content compresses to a
   smaller absolute size even though the compression ratio is slightly worse.

6. **Chunked upstream**: nginx's proxy module decodes chunked transfer encoding
   before body filters see the data. No issue.

## Issues

### P0: No protection against compressed origin responses — FIXED

~~No nginx config sets `proxy_set_header Accept-Encoding "";`.~~ **Fixed**: All
proxy_pass locations now strip Accept-Encoding, header filter rejects
Content-Encoding responses, and worker checks for gzip magic bytes.

Previously, the client's `Accept-Encoding: gzip, br` was forwarded to the
origin. If the origin had gzip/brotli enabled:

- Origin responds with `Content-Encoding: gzip`, compressed body
- PageSpeed body filter records **compressed bytes** to cache
- Worker reads gzip'd HTML/CSS/JS → parse fails or produces garbage
- Future HIT: serves compressed bytes without Content-Encoding header →
  gzip filter double-compresses → client receives broken response

**Worker failure modes by content type** (verified in code):

| Content Type | Behavior on Gzip'd Input | Corrupt Variant Written? |
|---|---|---|
| HTML | HtmlParse produces garbage (lenient parser) | **YES** |
| JS | MinifyUtf8Js "succeeds" with errors | **YES** |
| CSS | MinifyCss returns failure, worker skips | No |
| Image | Decoder rejects (bad magic bytes) | No |

HTML and JS are the dangerous cases — they fail silently and write corrupt
variants to cache.

**Double-compression confirmed**: The HIT path (`ngx_pagespeed_module.cc:976-1155`)
never sets `Content-Encoding` on the response. If compressed bytes are cached,
the gzip filter sees `text/html` without `Content-Encoding` → compresses again →
client receives `gzip(gzip(data))`, decodes once, sees binary garbage.

**Cache poisoning**: Compressed and uncompressed writes use the same AlternateId
(mask 0x08). A second write (compressed) creates a new chain head in Cyclone,
shadowing the first (uncompressed). One compressed origin response poisons the
cache for ALL subsequent requests to that URL.

**Affected configs** (none set `proxy_set_header Accept-Encoding ""`):
- `deploy/nginx.conf.example`
- `tools/e2e/nginx-e2e.conf`
- `tools/http-compliance/nginx-compliance.conf`
- `tools/stress/nginx-stress.conf`
- `tools/measure-demo/nginx-measure.conf`

**Why it hasn't surfaced yet**: E2E/stress origins are simple nginx containers
serving static files without gzip enabled. Real-world origins commonly compress.

**Origin requirement**: The origin MUST be capable of serving uncompressed
content. Origins that only serve pre-compressed assets (e.g., S3 with gzip'd
objects) may return 406 or serve compressed bytes regardless.

### P0b: Body filter has no recording size limit — FIXED

~~The body filter copies every buffer chunk into pool-allocated memory with no
upper bound.~~ **Fixed**: `kMaxRecordingSize` (10MB) limits accumulated
recording. When exceeded, recording stops but response is still served to
client. `ctx->done` is set to prevent wasteful re-entry.

### P1: Misleading comments — FIXED

~~`tools/http-compliance/test_encoding.py:9-11` says HIT "bypasses nginx's gzip
module".~~ **Fixed**: Updated to accurately describe that both HIT and MISS
pass through `ngx_http_output_filter` including the gzip filter.

~~`deploy/nginx.conf.example:34` says "for non-cached responses".~~ **Fixed**:
Updated to "applied to both cached (HIT) and proxied (MISS) responses".

**Note on `gzip_proxied`**: This directive is irrelevant for HITs because the
precontent-phase handler generates the response directly (not a proxied response in
nginx's view). `gzip_proxied any` only affects MISS (proxied) responses.

### P1b: No ETag / conditional response support on cache HITs

The HIT path does not emit `ETag` or `Last-Modified` headers and does not check
`If-None-Match` / `If-Modified-Since`. Every HIT returns a full 200 even if the
browser has a valid cached copy. This wastes bandwidth for repeat visitors.

### P1c: Range request support on HITs untested

The HIT path does not set `r->allow_ranges = 1`. nginx's built-in range filter
may still work via the `content_length_n` fallback, but this is nginx-version-
dependent and not tested in the compliance suite.

### P2: No brotli support

No brotli module configured anywhere. Brotli achieves ~15-25% better
compression than gzip for text. The module emits `Vary: Accept-Encoding` but
only gzip is available. All modern browsers support brotli.

**Load order matters**: When adding `ngx_brotli`, the module load order in
`nginx.conf` determines filter chain position. PageSpeed must load AFTER brotli
(`load_module ngx_http_brotli_filter_module.so;` before
`load_module ngx_pagespeed_module.so;`) so PageSpeed's body filter sits at the
top of the chain and captures uncompressed bytes before brotli compresses.

### P3: Dynamic compression on every HIT

Every cache HIT dynamically compresses through nginx's gzip filter.

**Quantified CPU cost** (gzip level 6):

| Content | Size | gzip time | At 10K req/s |
|---------|------|-----------|--------------|
| Minified HTML | 50 KB | ~0.3-0.5 ms | ~3-5 CPU cores |
| CSS file | 200 KB | ~1.2-2.0 ms | ~12-20 CPU cores |
| JS bundle | 500 KB | ~3-5 ms | ~30-50 CPU cores |

For high-traffic deployments, this is material. Images are unaffected
(`gzip_types` excludes image formats).

#### AlternateId Bit Space Redesign (PROPOSED)

**Current layout** (8 bits, all consumed) — UPDATED in Phase 4.1:

```
Bits 0-1: Image Format       (Original/WebP/AVIF/JXL)          2 bits
Bits 2-3: Viewport Class     (Mobile/Tablet/Desktop/Sentinel)   2 bits
Bit 4:    Pixel Density      (1x/2x+)                          1 bit
Bit 5:    Save-Data          (off/on)                           1 bit
Bits 6-7: Transfer Encoding  (Identity/Gzip/Brotli/Reserved)   2 bits
```

Default mask: `CapabilityMask()` = 0x08 (Desktop/Identity).

**Connection type was replaced** (rationale follows). Investigation revealed:

1. **Lowest scoring weight**: Connection contributes only 10 points to the
   selector score (`pagespeed_selector.cc:75-78`), vs format(1000),
   viewport(80), density(40), save-data(20).

2. **Semi-broken in proactive generation**: The worker's proactive variant loop
   (`worker.cc:1275-1373`) iterates formats × viewports × densities × save-data
   but does NOT iterate connection types. All 36 proactive variants inherit the
   triggering client's `target_mask.connection_type()` (line 1293, 1310).
   The first client to trigger processing determines the connection-based quality
   for ALL variants of that image. Subsequent clients with different ECT values
   get the "wrong" quality level.

3. **Redundant with Save-Data**: Save-Data already captures the "user wants
   less data" signal. Connection-based quality factors (50-85% via
   `--ect-*-factor` flags) are a refinement that Save-Data quality settings
   (`--savedata-*-quality` flags) already cover for the explicit opt-in case.

4. **Not a variant dimension in practice**: Since connection doesn't multiply
   variant count, it only affects quality at transcode time for the single
   triggering request. Other clients with different ECT get whatever quality
   was used.

**Implemented layout** (Phase 4.1, replacing connection with transfer encoding):

```
Bits 0-1: Image Format       (Original/WebP/AVIF/JXL)          2 bits
Bits 2-3: Viewport Class     (Mobile/Tablet/Desktop/Sentinel)   2 bits
Bit 4:    Pixel Density      (1x/2x+)                          1 bit
Bit 5:    Save-Data          (off/on)                           1 bit
Bits 6-7: Transfer Encoding  (Identity/Gzip/Brotli/Reserved)   2 bits
```

**Sentinel mechanism**: Unchanged. Viewport=3 (bits 2-3 = 0b11) remains the
sentinel indicator. `IsSentinel()` logic is unaffected.

**Variant counts per URL type** (within Cyclone's 64-alternate limit):

| URL Type | Variants | Calculation | + Sentinels | Total |
|----------|----------|-------------|-------------|-------|
| Image | 36 | 3 fmt × 3 vp × 2 den × 2 sd × 1 enc (identity only) | ~7 | 43 |
| CSS | 3 | 1 minified × 3 encodings | ~7 | 10 |
| JS | 3 | 1 minified × 3 encodings | ~7 | 10 |
| HTML | 18 | 3 vp × 2 sd × 3 encodings | ~7 | 25 |

All well within the 64-alternate limit. Images stay at identity encoding
(already compressed formats — gzipping a WebP/AVIF/JPEG is counterproductive).

**What was removed** (Phase 4.3): Per-ECT quality tuning via `--ect-3g-factor`,
`--ect-2g-factor`, `--ect-slow2g-factor`. These CLI flags and the `ConnectionType`
enum have been removed. Save-Data quality settings (`--savedata-*-quality`) remain
and serve the explicit data-saving intent.

**What was gained** (Phase 4.1-4.3, enabling 4.4+):
- Bit space for pre-compressed alternates for ALL text resources (HTML, CSS, JS)
- Selector correctly handles encoding scoring with hard disqualification
- Accept-Encoding parsing in CapabilityMask with q=0 support
- nginx classify function reads Accept-Encoding (not ECT)
- Foundation for worker gzip/brotli variant production (Phase 4.4)
- Foundation for nginx pre-compressed serving (Phase 4.5)

**Selector encoding scoring** (reviewed and corrected by council):

Encoding MUST NOT use a high weight (+500 was proposed but REJECTED). A high
encoding weight causes the selector to prefer a wrong-viewport pre-compressed
variant over a correct-viewport identity variant. Example: Desktop/brotli client,
cache has Mobile/brotli and Desktop/identity → Mobile/brotli would win at +500,
serving wrong critical CSS to a desktop browser.

Instead, **treat mismatched non-identity encoding as a hard disqualification**:
- Exact encoding match: +60
- Stored = identity (always decodable, dynamic compression covers): +5
- Stored = different non-identity encoding (client cannot decode): **score 0
  total** (disqualify — analogous to never serving AVIF to a WebP-only client)

This ensures correctness dimensions (format, viewport, density) always dominate,
while encoding is a "nice to have" preference with safe fallback to identity.

**Accept-Encoding parsing rules** (from nginx/security review):
- Priority: `br` > `gzip` > `identity` (match image format waterfall pattern)
- Respect `q=0` rejections: `gzip;q=0` means client does NOT accept gzip
- `Accept-Encoding: *` → map to kBrotli (strongest available)
- Absent header → kIdentity
- Read from `r->headers_in` (client request), NOT confused with the P0
  `proxy_set_header Accept-Encoding ""` which only affects upstream requests

**HTML variant count note**: The 18 HTML variants (3 vp × 2 sd × 3 enc) is
the theoretical max. In practice, HTML variants are demand-driven (not proactive
like images). Most sites see 3-6 active viewport/save-data combinations. Save-Data
does not currently affect HTML processing (transforms are identical regardless of
save-data bit). Consider collapsing save-data for HTML to reduce from 18 to 9 max.

**Key constraints verified**:
- AlternateId is uint8_t in Cyclone wire format (`document.hpp:76`) — cannot widen
- Max 64 alternates per key (`alternate.hpp:30`) — all URL types fit
- Cyclone's named AlternateId values (Brotli=1, Gzip=3) are irrelevant to
  PageSpeed (`alternate_id.h:16-20`) — PageSpeed owns the semantics
- Sentinel space unaffected: different encoding bits at positions 6-7 do not
  collide with viewport=3 sentinel detection at bits 2-3
- No backwards compatibility concern (per user)
- 1-bit connection + 1-bit encoding alternative REJECTED: would push image
  variants to 72 (exceeds 64-alternate limit), forces "which format?" ambiguity

### P4 (minor): Body filter skips file-backed buffers

`ngx_pagespeed_module.cc:780-781` silently skips `b->in_file` buffers. With
`proxy_buffering on` (default) this doesn't happen, but with buffering off,
the cache could receive incomplete content.

## Implementation Plan

### Phase 1: P0 fix — COMPLETE

All four defenses implemented and council-reviewed:

1. **Module-level guard** (`ngx_pagespeed_module.cc`): Header filter checks
   `r->headers_out.content_encoding`, sets `ctx->uncacheable = 1`, logs warning
   with `proxy_set_header` fix instructions. Council: APPROVED WITH NOTES.

2. **Config-level strip**: `proxy_set_header Accept-Encoding "";` added to all
   8 proxy_pass locations across 8 config files (deploy, helm, e2e, compliance,
   stress, measure-demo, sample, cache-tests). Bypass locations (`/api/`,
   `/admin/` with `pagespeed off`) correctly excluded. Council: APPROVED after
   iteration (2 missed configs found and fixed).

3. **Worker magic byte guard** (`worker.cc`): HTML, CSS, JS paths all check
   first 2 bytes for `0x1f 0x8b` before parsing. Error log includes remediation
   hint. Council: APPROVED after iteration (CSS/JS messages aligned with HTML).
   Note: unit tests deferred to separate commit.

4. **Body filter size limit** (`ngx_pagespeed_module.cc`): `kMaxRecordingSize`
   (10MB) with `recorded_bytes` tracking in ctx. Sets both `ctx->recording = 0`
   and `ctx->done = 1` when exceeded. Council: APPROVED WITH NOTES (done flag
   fix applied per review).

**E2E regression test (1.5)**: Pending — separate commit.

### Phase 2: Fix comments + config tuning — COMPLETE

1. **Comment corrections**: `test_encoding.py` docstring and
   `nginx.conf.example` gzip comment updated to accurately describe that both
   HIT and MISS paths pass through `ngx_http_output_filter`. Council: APPROVED.

2. **Gzip tuning**: `gzip_comp_level` 6→4, added `gzip_min_length 256`.
   Council (performance): APPROVED — level 4 is industry-standard for edge
   proxies (~40% faster, ~2-3% larger).

### Phase 3: Brotli support (future)

- Add `ngx_brotli` module to the Docker build.
- **Load order**: `load_module brotli` BEFORE `load_module pagespeed` so
  PageSpeed's body filter sits above brotli's in the filter chain.
- Use brotli level 4 for dynamic compression (level 6+ is ~4x slower than
  gzip-6 per byte — too expensive for per-HIT use).
- For the MISS path, brotli level 4 is acceptable since MISS is a one-time
  cost.
- Add compliance tests for brotli.

### Phase 4: AlternateId bit space redesign + pre-compressed alternates

**Replace connection bits (6-7) with transfer encoding bits.** This is the
enabling change for pre-compressed alternates across all text resource types.

#### Must-fix implementation invariants (from council review)

**I1: Content-Encoding on pre-compressed HIT (CRITICAL)**. When serving a
non-identity alternate, the module MUST set `r->headers_out.content_encoding`
(the pointer, not just a list entry). This is what nginx's gzip and brotli
filter modules check to suppress double-compression. If this pointer is not
set, the filter compresses already-compressed bytes → client receives garbage.
This is the Phase 4 analogue of the P0 bug. Must have a dedicated E2E test.

```cpp
// In HIT path, after reading AlternateMetadata:
uint8_t enc_bits = (meta.full_mask >> kEncodingShift) & kEncodingMask;
if (enc_bits == kGzip || enc_bits == kBrotli) {
    ngx_table_elt_t* ce = ngx_list_push(&r->headers_out.headers);
    if (ce) {
        ce->hash = 1;
        ngx_str_set(&ce->key, "Content-Encoding");
        ngx_str_set(&ce->value, enc_bits == kBrotli ? "br" : "gzip");
        r->headers_out.content_encoding = ce;  // MUST set the pointer
    }
}
```

**I2: Encoding mismatch = hard disqualification**. The selector MUST return
score 0 (total) for alternates with a non-identity encoding that does not
match the client's encoding. Serving brotli to a gzip-only client produces
undecodable content. Identity is always safe (dynamic compression covers).

**I3: Identity fallback is the current behavior**. When the selector falls
back to an identity alternate (no matching pre-compressed variant), do NOT
set Content-Encoding. nginx's gzip/brotli filters run normally via
`ngx_http_output_filter` — this is exactly the existing (pre-Phase-4) flow.

#### Code changes required

1. **`lib/classify/capability_mask.h`**: Replace `ConnectionType` enum with
   `TransferEncoding` enum (`kIdentity=0, kGzip=1, kBrotli=2, kReserved=3`).
   Replace `connection_type_` field and accessors. Update `Encode()`/`Decode()`.
   Update `FromHeaders()` to accept `accept_encoding` parameter.
   Remove `kConnectionShift`/`kConnectionMask`, add `kEncodingShift`/`kEncodingMask`.

2. **`lib/classify/pagespeed_selector.cc`**: Replace connection scoring with
   encoding scoring. **Exact match: +60. Identity fallback: +5. Mismatched
   non-identity: return 0 total (hard disqualify).** This ensures correctness
   dimensions (format +1000, viewport +80) always dominate.
   Remove `ExtractConnection()`, add `ExtractEncoding()`.

3. **`lib/classify/capability_mask.cc`**: Update `FromHeaders()`:
   - Parse `Accept-Encoding` multi-value header
   - Priority: `br` > `gzip` > `identity`
   - Respect `q=0` rejections (`gzip;q=0` → do not select gzip)
   - `Accept-Encoding: *` → kBrotli; absent → kIdentity
   - Update default `CapabilityMask()` to use kIdentity.

4. **`src/nginx/ngx_pagespeed_module.cc`**:
   - In `classify()`: parse `Accept-Encoding` from `r->headers_in` via
     `ngx_http_pagespeed_get_header()` and pass to `FromHeaders()`.
     (This is the CLIENT header — no confusion with the P0 upstream strip.)
   - In HIT path: extract encoding bits from `meta.full_mask`. If non-identity,
     set `r->headers_out.content_encoding` (invariant I1). If identity, let
     nginx's filter chain handle compression (invariant I3).
   - Re-notification mask (`ctx->mask->Encode()`, line 1114) includes encoding
     bits. Worker must treat encoding bits as informational, not prescriptive —
     worker decides which encodings to produce.

5. **`src/worker/worker.cc`**: After producing optimized text content
   (HTML/CSS/JS), also produce gzip and brotli variants:
   - Gzip: level 6 (good balance; level 9 has marginal benefit)
   - Brotli: level 6 (default; configurable via `--brotli-level`)
     - Level 11 REJECTED by council: ~10-20x slower than level 6, consumes
       ~37% of thread pool under burst load. Level 6 is ~5-8% larger but
       10x faster. Level 11 is only viable for low-traffic sites.
     - A later burst measurement (300 small text resources, copies of 12 demo
       HTML/CSS/JS files, queued at once on a 4-thread pool) found level 10
       drained the queue about 2.6x slower than level 6 (0.92 s against
       0.35 s) for roughly 10% smaller brotli output, so the default stays at
       6 and higher levels remain an opt-in.
   - Write each with the appropriate encoding bits in the mask
   - Images: always write with encoding=identity (skip compression)
   - Write order: identity first, then gzip, then brotli (so identity is
     available immediately for fallback)

6. **`src/worker/image_transcoder.cc`**: Remove ECT quality factor logic
   (lines 874-885). Remove `--ect-*-factor` CLI flags.

7. **Serve-path defense-in-depth**: Before serving a pre-compressed alternate,
   optionally verify magic bytes (gzip: `0x1f 0x8b`; brotli: heuristic on
   first byte). Log error if mismatch between encoding bits and actual content.

8. **Tests**: Update `pagespeed_selector_test.cc` (192-mask combinatorial test),
   `capability_mask_test.cc`, E2E tests. Add dedicated E2E test: serve brotli
   alternate → verify `Content-Encoding: br` → verify client decodes correctly.

9. **CLAUDE.md**: Update bit layout documentation, remove connection references,
   add encoding references.

**Migration**: No backwards compatibility needed. Cache can be purged on deploy.

**Default mask change** (DONE): `CapabilityMask()` default changed from 0xC8
(Desktop/4G+) to 0x08 (Desktop/Identity). All references updated in source,
tests, comments, and MEMORY.md.

#### Deployment notes

- **Highest ROI when PageSpeed is the edge** (no CDN in front): eliminates
  3-50 CPU cores of dynamic compression at 10K req/s.
- **Behind a CDN**: reduces origin-to-CDN bandwidth; CDN avoids re-compressing.
  Lower ROI since CDN caches and compresses independently.
- **During cold start**: first requests fall back to dynamic compression (current
  behavior) until worker produces compressed variants. No regression.

### Phase 5: ETag / conditional responses on HITs (future)

- Generate ETag from cache content hash or Cyclone alternate metadata.
- Check `If-None-Match` in the HIT path before building the response.
- Return 304 when the client's cached copy is valid.
- Set `r->allow_ranges = 1` on the HIT path for proper range support.
- Add compliance tests for conditional requests and range requests on HITs.

### Phase 6: Observability (future)

- Add nginx-side counters: `compressed_origin_skipped`, `recording_failures`,
  `recording_success`.
- Expose via shared memory zone or stub status endpoint.
- Add worker-side counter: `compressed_origin_detected` (canary for the bug).
- Document log pattern alerting:
  `"pagespeed: upstream response has Content-Encoding"`.
- Consider a management socket command to scan alternates for gzip magic bytes
  in HTML/CSS/JS entries (cache integrity verification).

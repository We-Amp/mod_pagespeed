# Nginx Module

Built as a dynamic `.so` via Bazel. Targets tagged `manual` (skipped by
`bazel build //...`).

```bash
NGINX_PATH=/path/to/nginx-source bazel build //src/nginx:ngx_pagespeed_module.so
```

`bazel/nginx.bzl` auto-detects nginx source from: `NGINX_PATH` env var,
`third_party/nginx` submodule, or `/usr/local/src/nginx` fallback. Generates
stub headers if nginx hasn't been `./configure`d.

## Response Header Handling

Headers inspected before recording to cache:

| Header | Behavior |
|--------|----------|
| `Cache-Control: no-store/private` | NOT cached, no worker notification |
| Request carries `Authorization` + response CC lacks `public`/`must-revalidate`/`s-maxage` | NOT cached, no worker notification (RFC 9111 §3.5) |
| `Cache-Control: no-transform` | Cached but NO worker notification |
| `Set-Cookie` | NOT cached (implicitly private) |
| `Vary: *` or any Vary token other than `Accept-Encoding` (incl. `User-Agent`, `Accept`, `Save-Data`, `Origin`) | NOT cached: one original per URL, so it would reach clients that sent another value |
| `Vary: Accept-Encoding` | Allowed (identity stored, coding chosen per request) |
| HEAD requests | NOT cached |
| Any origin header a cache hit cannot reproduce (`ClassifyForHeadersSidecar` fall-through, e.g. `X-Request-Id`) | NOT cached, stored entry for the URL removed, `headers_sidecar_fall_through` counted, one NOTICE per (host, header) |
| `Content-Security-Policy` carrying a nonce | NOT cached, stored entry removed, `headers_sidecar_never_optimized` counted |
| `Link` carrying a nonce (any spelling of the parameter, outside the `<...>` targets) | NOT cached, stored entry removed, `headers_sidecar_never_optimized` counted (reason `nonce-link`) |
| Only reproducible headers (allowlisted, metadata-reproduced, or stamped per response) | Cached; allowlisted lines stored in the `0x6C` sidecar |

Flags set in header filter, checked in body filter. HIT path unaffected —
except by the Authorization gate, which also gates the HIT path (see below).

**Response-header sidecar, store side** (issue #208; decisions in
`src/nginx/headers_sidecar_record.h`). The header filter snapshots the
ORIGIN's header lines (`ctx->origin_headers`) before the module adds its own
(X-PageSpeed, Accept-CH, x-verified-bot, Vary), and the gate runs on that
snapshot last among the MISS-block gates (200, non-HEAD, not already
uncacheable). `record_response` writes the sidecar FIRST and only then the
entry, flagged `kFlagHeadersClassified` (+ `kFlagHeadersSidecarRequired` when a
block was stored); a failed block write stores no entry
(`headers_sidecar_write_failed`). With nothing to carry the entry is flagged
classified only and a leftover block is removed. The "stored exists, skip
write" shortcut is bypassed for a pre-gate entry, a required-but-missing block
(self-heal), a changed block, or a carry-state switch (which removes the whole
URL first, because worker variants carry the old bits); the last three are
single-flighted through the per-URL refresh slot. The 304 path merges the
stored block with the 304's lines (RFC 9111 §4.3.4): same block → restamp;
same carry state, new block → rewrite block then restamp (block write failure
skips the restamp); refusal / carry switch / unreadable required block → remove
the URL and serve the response. The rewritten 200 adds the stored lines the
304 did not carry. Hits replay the block (serve side, below); entries without
the classified bit serve exactly as before.

## HIT Path

**Response-header sidecar, serve side** (issue #208; decisions in
`src/nginx/headers_sidecar_replay.h`). For an entry flagged
`kFlagHeadersClassified | kFlagHeadersSidecarRequired` (and not the agent
markdown variant), the handler reads the URL's `0x6C` block by exact id once,
after the §3.5 / variant-format gates and before the freshness switch, and
copies its lines into the pool (`ctx->sidecar_fields`). An unreadable block
makes the request a MISS (`headers_sidecar_missing`), never stashed for
revalidation or stale-if-error, and the record arm self-heals the block.
Every serving arm replays the lines verbatim, in stored order, BEFORE its own
Vary / Cache-Control / Age / ETag (`emit_served_vary`, also on the coalesced
stale `goto serve_hit`); `headers_sidecar_replayed` counts responses that
sent ≥ 1 line. Vary: the origin's line first, then the computed line without
Accept when the block carried a Vary (the MISS shape). SVG: the module's
`X-Content-Type-Options` is skipped when the block carried one (its CSP is
still added). Stale-if-error drops the error page's allowlisted lines for
every classified stash (with or without a block) and replays any stored ones;
a required stash without its block passes the error through. A block naming a
field the serve path builds (damaged cache file) is a logged MISS in every
build, never an abort. Legacy (unclassified) entries: no read, no replay, and
at their next genuine expiry they take one full re-fetch instead of a
conditional request (`ConditionalRevalidationAllowed`) so the re-record
classifies them. There is no limiter across URLs for this migration: a cache
warmed in one burst (e.g. after a purge) re-records in one burst at its
expiry, a full fetch plus a worker rebuild per URL; the per-URL refresh slot
and each entry's own expiry are the only mitigations.

**Content-Type**: Uses `AlternateMetadata.origin_content_type` (preserves charset).
Transcoded images use format-specific MIME. Falls back to `MimeFromUrl`.

**Content-Encoding**: When serving a pre-compressed variant (gzip/brotli),
decodes `meta.full_mask` to extract `TransferEncoding` and sets
`Content-Encoding: gzip` or `Content-Encoding: br`. Also assigns
`r->headers_out.content_encoding` to prevent nginx's built-in gzip and brotli
filters from double-compressing the already-compressed content.

**Cookie-bearing requests**: a request carrying `Cookie` or `Cookie2` (any
value, empty included) is handled by response class (`cookie_cache_gate.h`,
which calls the HTML cache policy's exported `CookieRequestMayStore` /
`CookieRequestMayServe`). A page — any type that is not css, JavaScript or an
image (`CookieShareClassForContentType`: case-insensitive, parameters
ignored, unknown = page) — is recorded, and served from the cache, only when
the origin Cache-Control carries `public` or an `s-maxage` above zero. A
static asset is recorded only with an origin-stated lifetime (max-age,
s-maxage, or Expires when no Cache-Control line was sent); stored assets are
served as before. The Markdown copy and the Early Hints set on a miss are
judged by the recorded page (the default-mask entry; none recorded = fail
closed); `/llms.txt` is never served to a cookie-bearing request. The 304
restamp declines to extend an entry once the rule is no longer met.
Independent of the Authorization gate below; a request carrying both passes
both. Entries stored before this rule are unreachable: the key namespace
moved to `ks3:`, and to `ks4:` with the Vary store rule
(`PageSpeedCache::kKeyNamespace`).

**Authorization (RFC 9111 §3.5)**: Authorization-bearing requests are served
from cache only when the stored entry's origin Cache-Control carries a §3.5
permit (`public`, `must-revalidate`, or `s-maxage`) — else treated as MISS.
Once stale, the two stale serves — the bounded coalesced-stale serve and the
stale-if-error serve — additionally require `public`: each consults
`AuthzCacheGateAllowsStale` at its own serve site (the stash-time gate only
proves the STORE permit). Early Hints on MISS are suppressed for
Authorization-bearing requests (see Early Hints below).
RSL-CAP exemption: with `rsl_cap_enforcement` on, `Authorization: License`
requests are exempt — derived at gate time from live enforcement state plus
the header's scheme (PREACCESS already validated-or-401'd them; no
per-request state). Main external requests only: never on internal requests
(`r->internal` — error_page/index/try_files redirect targets, subrequests),
which PREACCESS declines and therefore never License-validated; those are
gated as ordinary Authorization. Deliberate fail-closed trade-off: under
enforcement, topologies where every request reaching the module is internal
(`try_files ... @proxy` named-location proxying, `rewrite ... last`
front-controllers, X-Accel-Redirect targets) lose the License exemption, so
licensed-agent requests there fall to ordinary §3.5 gating and get the
cached lane only when the origin's cache directives permit it
(`public`/`must-revalidate`/`s-maxage`). A validated-flag design that would
restore the exemption for those topologies is tracked in a follow-up issue.
Canonical rationale: `src/nginx/authz_cache_gate.h`.

**ETag**: Cache HITs include a weak ETag in one of two shapes —
`W/"ps-<mask8><flags2>-<identity16>-<length16>"` when the entry carries a
content-identity signal (stored origin hash, or FNV over origin
ETag/Last-Modified), else the legacy `W/"ps-<mask8><flags2>-<length16>"`.
Canonical format description and rationale: `src/nginx/etag_util.h`.
Nginx's `not_modified` filter handles `If-None-Match` → 304 automatically.
Range requests enabled via `r->allow_ranges`. Weak ETags cause `If-Range` to
fall back to a full 200 (correct per RFC 9110).

**Cache-Control** on HIT uses the origin's Cache-Control directives stored in
`AlternateMetadata` (v3). The serving algorithm (RFC 9111 compliant):

1. **Recording** (MISS path): Parses all 10 CC directives (no-cache,
   must-revalidate, no-store, private, public, immutable, s-maxage,
   proxy-revalidate, no-transform, max-age) + Expires fallback into
   `AlternateMetadata` fields. Adjusts `cache_inserted_at` by inbound `Age`.
2. **Serving** (HIT path):
   - **Age** = `now - cache_inserted_at` (clamped to 0; corrupted → UINT32_MAX)
   - **Effective max-age** = `min(origin_max_age, pagespeed_max_age)` with
     immutable exception using `pagespeed_immutable_max_age`
   - **Stale + must-revalidate/proxy-revalidate/s-maxage** → falls through to
     MISS (origin re-fetch)
   - **no-store/private** → defensively served as `no-cache`
   - **no-cache** → passed through verbatim
   - **Type defaults** (no CC header): HTML=`no-cache`, CSS/JS=300s, Images=3600s
   - **SWR synthesis** (configurable): `stale-while-revalidate=min(eff_max_age, 86400)`
     suppressed when must-revalidate, proxy-revalidate, or s-maxage present
   - **Age header** emitted on all HIT responses

**Directives**:
- `pagespeed_max_age N;` — cap on origin max-age (default: 86400)
- `pagespeed_immutable_max_age N;` — cap for immutable content (default: 604800)
- `pagespeed_synthesize_swr on|off;` — SWR synthesis (default: on)
- `pagespeed_conditional_revalidation on|off;` — conditional revalidation (default: on)
- `pagespeed_html_max_age N;` — default max-age for HTML when origin sends no CC (default: 0)
- `pagespeed_css_max_age N;` — default max-age for CSS/JS when origin sends no CC (default: 300)
- `pagespeed_image_max_age N;` — default max-age for images when origin sends no CC (default: 3600)
- `pagespeed_force_refresh_html on|off;` — force revalidation on browser Ctrl+F5 for HTML (default: on)
- `pagespeed_force_refresh on|off;` — force revalidation on browser Ctrl+F5 for all non-HTML types (default: off)

**Vary headers** emitted on cache-served responses:
- Images: `Vary: Accept, Save-Data, User-Agent`
- HTML: `Vary: Accept-Encoding, User-Agent`
- CSS/JS: `Vary: Accept-Encoding`

## Conditional Revalidation

When a cached entry becomes stale and has stored ETag or Last-Modified tokens
(metadata v5), nginx sends `If-None-Match` / `If-Modified-Since` to origin
instead of a full re-fetch. On 304: refreshes metadata timestamps, copies
cached content to new alternate, evicts RAM cache, serves with
`X-PageSpeed: REVALIDATED`. On 200: discards stale, proceeds with normal MISS.

**Storability is re-evaluated on the 304** (issue #1016) before the
write-back, over the *merged* headers, in
`src/nginx/revalidation_storability.{h,cc}` — the same flag vocabulary the
record path checks, so both agree on "storable". Three verdicts: `kEvict`
(304 says `no-store` or **bare** `private` — the stored entry is removed for
the whole URL), `kSkipRestamp` (still storable, but this exchange may not
extend its life: §3.5 permit withdrawn, or the 304 carried `Set-Cookie` —
the entry keeps its old stamp and simply revalidates again), `kRestamp`
(unchanged). Deliberately not `kEvict` on the §3.5 path: an inbound request
header must never be able to flush another client's lawfully stored entry.
This gates retention only — the current requester is still served, which is
this branch's pre-existing behavior and an open product question, not
something the commit settled.

**Qualified `private` / `no-cache`** (`private="Set-Cookie"`, RFC 9111
§5.2.2.7) restrict only the named field-names and permit storing the
remainder. The parser records the FORM of each occurrence —
`kCCOriginPrivateBare` / `kCCOriginPrivateQualified` (and the `NoCache`
pair) — alongside the base flag, never instead of it, so every existing
reader keeps its conservative behavior. **Both directions are tracked
on purpose:** `cc_flags` is OR-accumulated across every token of every
`Cache-Control` header line, so a "qualified" bit alone would only mean
"some occurrence was qualified", and

```
Cache-Control: private="Set-Cookie"
Cache-Control: private
```

would skip an eviction the bare directive plainly demands. *Bare-seen* is
monotone under OR and is the load-bearing bit. Decision rule for any
destructive consumer: bare set → blanket; qualified set and bare clear →
qualified only; neither (legacy metadata) → treat as blanket, fail safe.
Classification keys on the presence of `=`, not a non-empty value, so
`private=""` (a legal empty field-list) is qualified. The MISS path still
refuses to store either form — deliberately unchanged.

**The 304 branch emits Cache-Control through the canonical
`BuildCacheControlHeader`**, not a local emitter, and sets two independent
flags:

- `relay_origin_no_cache` — **always**, on every 304 outcome. `no-cache` is
  about revalidation, not storage permission; it applies to plain restamps
  too. Gating it on the eviction predicate strips it from every
  `no-cache, max-age=N` response, converting a revalidate-before-every-reuse
  resource into one reusable for the whole max-age. (That conflation was a
  real regression — the two concerns are not the same predicate.)
- `forward_origin_restrictions` — only when the entry was **evicted**.
  Forwards `no-store` / bare `private`, suppresses the safe-mode
  `must-revalidate` auto-add, drops `max-age` under `no-store`, and
  suppresses `stale-if-error` / `stale-while-revalidate` (both are
  permission to serve a stale *stored* copy, which presupposes the storage
  being forbidden).

Both default off, so HIT-path output is byte-identical.

**Dispatch lives in `ApplyRestampVerdict`** (a template over the cache type
in `revalidation_storability.h`), so the cache effects — eviction actually
removing, skip actually not writing — are asserted against a real
`PageSpeedCache` in `test/src/nginx/restamp_apply_test.cc`. Production
instantiates the same code with the same type; no indirection. `Remove()`
returning `NotFound` counts as success (a purge got there first; that IS the
desired end state) — only a genuine refusal such as `NotOwned` is an error,
and it is logged rather than discarded.

Client conditional headers (`If-None-Match` with PageSpeed's weak ETag) are
saved before injection and restored on all exit paths (304, 200, error).
Stats: `g_conditional_revalidations`, `g_conditional_304s`,
`g_conditional_200s`, `g_etag_too_long`.

## Hot URL Warmup

Tracks fallback-hit frequency per URL in fixed-size hash map (4096 entries,
FNV-1a hash, probabilistic eviction). When threshold exceeded (default: 5,
`pagespeed_hot_threshold N;`), sends warmup notification with sentinel mask
`0xFFFFFFFE`.

## Early Hints (103)

On cache MISS for HTML, reads `SentinelId::kEarlyHints` alternate and sends
`103 Early Hints` with Link headers before proxying. Built against nginx
1.29.0 or newer, the 103 goes through nginx's own early-hints filter chain
(`ngx_http_top_early_hints_filter`), so it is framed correctly on HTTP/1.1,
HTTP/2 and HTTP/3; older nginx has no such API and only HTTP/1.1 gets a raw
103. The MISS-path lookup is keyed on content type
(`ShouldSendHintsOnMiss`): `.html` URLs, plus extensionless URLs when the
request's Accept names `text/html`. System test:
`tools/test-early-hints-protocols.sh`. Suppressed for
§3.5-effective Authorization-bearing requests: hints derive from a stored
response, the sentinel carries no metadata prefix (no CC flags), and the
main entry's metadata is unavailable or gate-refused at that point — so no
permit can be established and the emit fails closed. Also adds Link headers
on cache HIT for HTML (allowed: that serve already passed the §3.5 gate).
Hint lines use prefixes to determine Link header type:
- No prefix → `rel=preload; as=style` (stylesheet)
- `image:` prefix → `rel=preload; as=image; fetchpriority=high` (LCP image)
- `font:` prefix → `rel=preload; as=font; crossorigin` (font)
- `preconnect:` prefix → `rel=preconnect` (no-cors third-party origin)
- `preconnect-cors:` prefix → `rel=preconnect; crossorigin` (CORS-mode
  third-party origin)

An unknown prefix falls back to the bare stylesheet preload, so a new prefix
must land in every sentinel consumer (`ParseHintLine` and the .NET
middleware's `BuildLinkHeader`) in the same release, with consumers deployed
no later than the emitting worker — and rollback across a sentinel-format
change requires a cache purge.

## Cache Key Site

Every key is in the keyspace (site) of the server block that received the
request: `ngx_http_pagespeed_site(r)`, derived at configuration time from the
block's server names and listen addresses (`src/nginx/site_identity.h`). Every
URL-based cache call, the precomposed serve key and every `CacheNotification`
carry it; the record and revalidation helpers get it through
`SiteScopedCache`. The Host header only selects an entry inside the block's
own keyspace. Seeding a HIT test therefore names the block:
`seed_test_cache --site_listen 0.0.0.0:<port> --site_names localhost`
(t/seed-*.sh). Integration test: `t/112-cache-per-server-block.t`.

## Cache Key Scheme

The key scheme comes from the connection's TLS state for every protocol
(`ngx_http_pagespeed_scheme`), after a trusted `X-Forwarded-Proto`; the
request line and `:scheme` are never used. Manual system test (not run in CI):
`tools/test-scheme-cache-key.sh`, next to `tools/test-early-hints-protocols.sh`.

## Shared Config (`pagespeed-shared.conf`)

The nginx module reads `pagespeed-shared.conf` from `parent_of(cache_path)`
(i.e., the parent directory of the Cyclone cache file). The file is polled
every ~1 second for changes.

**Format:** `key=value` (one per line)

**Fields read by nginx:**
- `socket_path` — Unix socket for worker notifications
- `html_mode` (with the legacy `disable_html` as fallback, resolved by
  `ResolveHtmlMode` in `src/worker/html_mode.h`, the same rule as the
  optimizer's reader) — `off` sends no HTML notification and never re-notifies
  on an HTML hit (the old `disable_html=true`); `full` is the old
  `disable_html=false`; a `markdown-*` mode still sends the store notification
  but re-notifies on an HTML variant miss only for an agent request, since no
  HTML variant is ever written in that mode
- `cache_dir_generation` — the optimizer's compiled-in cache-directory
  generation, compared with the module's own (`kCacheDirGeneration`, shared,
  not copied) by `src/nginx/cache_generation_check.h`. Mismatch = pass-through
  with the cache off (the GetCache gate + `ctx->uncacheable` in the ACCESS
  handler), one `[error]` per change / config cycle; absent = unknown, cache
  used as before, one `[warn]`. Re-evaluated on every shared-config read, so
  recovery needs no reload. Exposed as `$pagespeed_cache_generation`,
  `$pagespeed_cache_generation_module`, `$pagespeed_cache_generation_optimizer`.
  Integration test: `t/106-cache-generation.t`.

This replaces the former `pagespeed_worker_socket` and `pagespeed_critical_css`
directives. Both are now configured on the worker side and shared automatically.
(There is no `license_key` any more: the license apparatus was removed at
2.1 GA.)

## `pagespeed_disallow` Directive

URL exclusion patterns (multiple allowed, first match wins):
- Prefix: `pagespeed_disallow /api/;`
- Suffix: `pagespeed_disallow *.woff2;`
- Substring: `pagespeed_disallow admin;`

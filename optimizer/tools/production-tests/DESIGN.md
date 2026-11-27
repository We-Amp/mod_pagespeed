# Production Readiness Test Suite — Design Document

## Overview

This test suite validates mod_pagespeed 2.1's production readiness through systematic
end-to-end testing covering HTTP protocol compliance, security, data integrity,
streaming, stress, and operational resilience.

**Total: ~175 tests across 10 modules**

## Architecture

```
┌─────────────┐     ┌───────────────┐     ┌──────────────────┐     ┌──────────────┐
│   pytest     │────▶│  nginx :8200   │────▶│  Dynamic Origin   │     │   Worker      │
│   client     │     │  (pagespeed)   │     │     :8201         │     │ (unix sock)   │
└─────────────┘     └───────┬───────┘     └──────────────────┘     └──────────────┘
                            │                                             │
                            └──────────── /shared volume ─────────────────┘
```

### Components

1. **Dynamic Origin** (`origin/`) — FastAPI server with programmable routes for
   every test scenario (status codes, delays, chunked, disconnect, encoding,
   conditional requests, range, cache-control directives)

2. **Nginx** — Standard PageSpeed nginx module config, proxying to origin on MISS

3. **Worker** — Standard factory worker processing notifications

4. **Test Client** (`conftest.py`) — Extended `ComplianceClient` with helpers for
   polling, hash verification, header assertions, and concurrent requests

## Dynamic Origin Server Design

The origin is the critical component. It must support:

### Programmable Response Routes

```
GET /status/{code}                    → Returns specified status code
GET /status/{code}?body=<text>        → With custom body
GET /status/{code}?cache=<seconds>    → With Cache-Control: max-age

GET /cc/no-store                      → Cache-Control: no-store
GET /cc/no-cache                      → Cache-Control: no-cache + ETag
GET /cc/private                       → Cache-Control: private
GET /cc/must-revalidate?max_age=N     → Cache-Control: must-revalidate, max-age=N
GET /cc/s-maxage?s=N&m=M             → Cache-Control: s-maxage=N, max-age=M
GET /cc/no-transform                  → Cache-Control: no-transform
GET /cc/immutable?max_age=N           → Cache-Control: immutable, max-age=N

GET /vary/{header}                    → Vary: {header}
GET /vary/multi?headers=a,b           → Vary: a, b

GET /etag/{type}/{id}                 → ETag: W/"id" or "id" (weak/strong)
GET /conditional/{id}                 → Supports If-None-Match → 304

GET /range/{id}                       → Supports Range requests + If-Range

GET /chunked/small                    → 1-byte chunks
GET /chunked/large                    → 1MB chunks
GET /chunked/slow?delay=N             → Chunks with N-second delay
GET /chunked/empty                    → Zero-length chunked body
GET /chunked/trailers                 → With trailer headers

GET /size/{bytes}?type=<mime>         → Exact-size response body
GET /size/10mb                        → At recording limit boundary
GET /size/10mb-plus-1                 → Just over recording limit

GET /delay/{seconds}                  → Delayed response (float seconds)
GET /delay/{seconds}?status=504       → Delayed with specific status

GET /disconnect/mid?after=N           → Closes connection after N bytes
GET /disconnect/headers-only          → Sends headers, closes before body

GET /encoding/gzip                    → Content-Encoding: gzip (pre-compressed)
GET /encoding/brotli                  → Content-Encoding: br (pre-compressed)
GET /encoding/identity                → Content-Encoding: identity (explicit)

GET /redirect/{code}?to=<url>         → Redirect with Location header
GET /redirect/301?cache=3600          → Cacheable redirect

GET /headers/set-cookie               → Response with Set-Cookie
GET /headers/cors                     → With CORS headers
GET /headers/security                 → With CSP, HSTS, X-Frame-Options
GET /headers/custom?h=<json>          → Custom response headers
GET /headers/link-preload             → Link: rel=preload header

GET /content/html                     → HTML with CSS/JS/image refs
GET /content/html-charset?c=utf-8     → HTML with charset
GET /content/html-bom                 → HTML with UTF-8 BOM
GET /content/css-import-chain?depth=N → CSS with @import N levels deep
GET /content/css-xss                  → CSS with </style><script>
GET /content/binary-nulls             → Binary with embedded \x00
GET /content/animated-gif             → Multi-frame animated GIF

GET /echo                             → Echoes request method, headers, body

GET /counter/{name}                   → Increments + returns request count
                                        (for verifying origin was hit N times)

POST /configure                       → Runtime route configuration
```

### Implementation: FastAPI + uvicorn

FastAPI provides:
- async handlers for streaming responses
- Easy route definition
- Built-in request/response model
- Startup/shutdown hooks for counters

## Docker Compose Stack

```yaml
# tools/production-tests/docker-compose.yml
services:
  origin:
    build:
      context: ./origin
    ports:
      - "${PRODTEST_ORIGIN_PORT:-8201}:8081"
    volumes:
      - shared:/shared

  worker:
    build:
      context: ../..
      dockerfile: tools/production-tests/Dockerfile.worker
    volumes:
      - shared:/shared
    environment:
      - PAGESPEED_CACHE_PATH=/shared/cache.vol
      - PAGESPEED_SOCKET_PATH=/shared/pagespeed.sock
      - PAGESPEED_MGMT_SOCKET_PATH=/shared/mgmt.sock
      - PAGESPEED_HEALTH_SOCKET_PATH=/shared/health.sock
      - PAGESPEED_CACHE_SIZE=1073741824
    depends_on:
      origin:
        condition: service_healthy

  nginx:
    build:
      context: ../..
      dockerfile: tools/production-tests/Dockerfile.nginx
    ports:
      - "${PRODTEST_NGINX_PORT:-8200}:8080"
    volumes:
      - shared:/shared
    depends_on:
      worker:
        condition: service_started

volumes:
  shared:
```

## Test Modules

### Module 1: `test_status_codes.py` (20 tests)

Tests HTTP status code forwarding through the proxy for both cached and
uncached paths.

**P0 (10 tests):**
- `test_200_ok_cached` — Basic cache flow: MISS → HIT
- `test_301_moved_permanently_cached` — Redirect with CC cached
- `test_302_found_not_cached_default` — Redirect without CC not cached
- `test_304_not_modified_passthrough` — Origin 304 not stored
- `test_400_bad_request_not_cached` — Client error not cached
- `test_404_not_found_not_cached_default` — 404 without CC not cached
- `test_404_not_found_cached_with_cc` — 404 with max-age IS cached
- `test_500_internal_error_not_cached` — Server error not cached
- `test_502_bad_gateway_not_cached` — Upstream error not cached
- `test_503_service_unavailable_not_cached` — Service unavailable not cached

**P1 (6 tests):**
- `test_201_created_post_not_cached` — POST response not cached
- `test_204_no_content_not_cached` — No content not cached
- `test_307_temporary_redirect_cached_with_cc` — 307 with CC cached
- `test_308_permanent_redirect_cached` — 308 cached
- `test_410_gone_cached_with_cc` — 410 cached with CC
- `test_429_too_many_requests_not_cached` — Rate limit not cached

**P2 (4 tests):**
- `test_403_forbidden_cached_with_cc` — 403 with explicit CC cached
- `test_405_allow_header_preserved` — Allow header preserved
- `test_429_retry_after_preserved` — Retry-After header preserved
- `test_504_gateway_timeout` — Origin timeout handling

### Module 2: `test_conditional_requests.py` (15 tests)

Tests If-None-Match, If-Modified-Since, ETag generation, 304 responses.

**P0 (8 tests):**
- `test_if_none_match_weak_etag_304` — Weak ETag match → 304
- `test_if_none_match_wrong_etag_200` — Wrong ETag → 200 with body
- `test_if_none_match_multiple_etags_match` — Match in list of ETags
- `test_if_none_match_star_304` — Wildcard match → 304
- `test_etag_format_is_weak` — ETag starts with W/"ps-
- `test_etag_consistency_same_variant` — Same variant always returns same ETag
- `test_etag_different_per_variant` — Different variants have different ETags
- `test_304_has_correct_headers` — 304 includes ETag, Age, CC

**P1 (5 tests):**
- `test_if_none_match_strong_vs_weak_comparison` — Strong sent, weak stored
- `test_if_modified_since_not_modified_304` — Date comparison
- `test_if_modified_since_without_last_modified_200` — No LM → 200
- `test_combined_conditions_etag_wins` — INM takes precedence
- `test_head_request_conditional_304` — HEAD + INM → 304

**P2 (2 tests):**
- `test_if_modified_since_invalid_date_ignored` — Bad date → 200
- `test_etag_stability_across_purge_refetch` — Same ETag after re-cache

### Module 3: `test_cache_control.py` (18 tests)

Tests Cache-Control directive handling for both response and request directives.

**P0 (10 tests):**
- `test_max_age_respected` — Cached for max-age seconds
- `test_no_store_not_cached` — no-store bypassed entirely
- `test_private_not_cached` — private bypassed (shared cache)
- `test_no_cache_requires_revalidation` — Cached but always revalidates
- `test_no_transform_original_format_only` — No WebP/AVIF variants
- `test_age_header_present_on_hit` — Age header on HIT responses
- `test_age_header_increments` — Age increases over time
- `test_stale_response_not_served_with_must_revalidate` — 504 when stale + unreachable
- `test_s_maxage_overrides_max_age` — Shared cache uses s-maxage
- `test_max_age_zero_triggers_revalidation` — Stale immediately

**P1 (5 tests):**
- `test_immutable_long_cache` — Immutable flag honored
- `test_no_cache_and_max_age_interaction` — no-cache overrides max-age
- `test_request_no_cache_forces_revalidation` — Client no-cache
- `test_request_no_store_bypass` — Client no-store
- `test_swr_synthesis` — stale-while-revalidate added

**P2 (3 tests):**
- `test_request_max_age_zero` — Client max-age=0
- `test_request_only_if_cached_504` — only-if-cached on miss
- `test_cache_control_public_cached` — public + max-age

### Module 4: `test_range_requests.py` (12 tests)

Tests byte range serving on cached content.

**P0 (4 tests):**
- `test_single_byte_range_206` — Basic range → 206
- `test_unsatisfiable_range_416` — Beyond EOF → 416
- `test_range_with_if_none_match_304` — INM takes precedence over Range
- `test_content_range_header_correct` — Content-Range format

**P1 (5 tests):**
- `test_suffix_range` — bytes=-2048
- `test_open_ended_range` — bytes=5000-
- `test_middle_range` — bytes=5000-6000
- `test_range_with_weak_if_range` — Weak ETag in If-Range (must ignore)
- `test_range_on_miss_proxied` — Range on uncached content

**P2 (3 tests):**
- `test_invalid_range_syntax_200` — Bad syntax → full body
- `test_range_start_greater_than_end_200` — Inverted → full body
- `test_multipart_byte_ranges` — Multiple ranges

### Module 5: `test_content_negotiation.py` (16 tests)

Tests Accept-Encoding, Accept (image format), Vary, viewport, Save-Data.

**P0 (6 tests):**
- `test_accept_encoding_gzip_served` — Gzip variant on HIT
- `test_accept_encoding_brotli_served` — Brotli variant on HIT
- `test_accept_encoding_missing_identity` — No AE → identity
- `test_vary_cookie_not_cached` — Vary: Cookie → bypass
- `test_vary_star_not_cached` — Vary: * → bypass
- `test_image_format_webp_served` — Accept: image/webp → WebP variant

**P1 (7 tests):**
- `test_accept_encoding_quality_values` — br;q=1.0, gzip;q=0.5
- `test_accept_encoding_identity_fallback` — No compressed variant yet
- `test_image_format_avif_served` — Accept: image/avif → AVIF
- `test_image_format_original_fallback` — No modern format → original
- `test_viewport_mobile_classification` — Mobile UA → mobile variant
- `test_viewport_desktop_classification` — Desktop UA → desktop variant
- `test_save_data_header` — Save-Data: on affects variant selection

**P2 (3 tests):**
- `test_vary_accept_encoding_allowed` — Vary: AE from origin OK
- `test_vary_user_agent_not_cached` — Vary: UA → bypass
- `test_accept_encoding_empty_string` — Empty AE → identity

### Module 6: `test_streaming_transfer.py` (25 tests)

Tests chunked transfer, body size limits, buffer management, encoding.

**P0 (12 tests):**
- `test_chunked_small_chunks` — 1-byte chunks reassembled
- `test_chunked_large_chunks` — 1MB chunks
- `test_chunked_empty_body` — Zero-length chunked
- `test_body_exactly_10mb_cached` — At recording limit
- `test_body_10mb_plus_1_not_cached` — Over limit → not recorded
- `test_body_100mb_served_not_cached` — Large → passthrough only
- `test_body_zero_bytes` — Empty body cached
- `test_body_single_byte` — Minimum size
- `test_body_binary_with_nulls` — Null bytes preserved
- `test_pre_compressed_gzip_rejected` — CE: gzip from origin → not cached
- `test_pre_compressed_brotli_rejected` — CE: br from origin → not cached
- `test_content_length_matches_body` — CL == actual bytes on every response

**P1 (9 tests):**
- `test_chunked_slow_delivery` — 2s between chunks, no timeout
- `test_chunked_rapid_fire` — 100 chunks instant
- `test_sse_not_cached` — text/event-stream bypassed
- `test_origin_disconnect_mid_response` — Partial not cached
- `test_headers_only_then_disconnect` — No body → error
- `test_client_gzip_origin_identity` — MISS: identity, HIT: gzip
- `test_file_backed_buffer_activation` — Large response triggers file buffers
- `test_content_encoding_identity_explicit` — CE: identity from origin
- `test_keep_alive_multiple_requests` — 3 requests on same connection

**P2 (4 tests):**
- `test_chunked_with_extensions` — Chunk extensions ignored
- `test_chunked_with_trailers` — Trailer headers
- `test_http_pipelining` — Pipelined requests
- `test_content_length_mismatch` — CL says X, body is Y

### Module 7: `test_headers_security.py` (22 tests)

Tests header preservation, security headers, XSS in transforms, Set-Cookie.

**P0 (12 tests):**
- `test_cors_allow_origin_preserved` — CORS header survives cache
- `test_cors_preflight_not_cached` — OPTIONS not cached
- `test_csp_header_preserved` — CSP survives cache
- `test_hsts_header_preserved` — HSTS survives cache
- `test_x_frame_options_preserved` — XFO survives cache
- `test_set_cookie_not_cached` — Response with Set-Cookie not cached
- `test_set_cookie_stripped_from_cache` — If somehow cached, cookie removed
- `test_critical_css_xss_script_tag` — </style><script> escaped
- `test_critical_css_xss_url_protocol` — javascript: stripped from CSS
- `test_svg_sanitize_script_tags` — <script> stripped from generated SVG
- `test_svg_sanitize_external_refs` — External resources stripped from SVG
- `test_no_response_mixing_under_load` — Client A never gets client B's response

**P1 (7 tests):**
- `test_custom_origin_headers_preserved` — X-Custom-* preserved
- `test_content_disposition_preserved` — attachment/inline preserved
- `test_link_preload_no_duplicate` — No doubled Link headers
- `test_x_pagespeed_header_present` — X-PageSpeed: HIT|MISS
- `test_url_null_bytes_rejected` — Null in URL → 400
- `test_url_crlf_injection_rejected` — CRLF in URL → 400
- `test_request_smuggling_cl_te` — Dual CL+TE → rejected

**P2 (3 tests):**
- `test_via_header_appended` — Via chain
- `test_x_forwarded_for_appended` — XFF preserved
- `test_management_api_wsocket_limit` — WebSocket DoS protection

### Module 8: `test_stress_integrity.py` (20 tests)

Tests data integrity under concurrent load, thundering herd, body verification.

**P0 (8 tests):**
- `test_response_body_hash_under_load` — SHA-256 verify 1000 responses
- `test_content_length_accuracy_under_load` — CL matches body on 1000 req
- `test_response_mixing_isolation` — Unique content per URL, verify no mixing
- `test_thundering_herd_coalescing` — 100 concurrent requests for 1 URL
- `test_concurrent_variant_read_write` — Read while worker writes variants
- `test_compressed_variant_fidelity` — Decompress gzip/br → matches identity
- `test_etag_consistency_under_load` — Same variant → same ETag
- `test_alternate_list_integrity` — All variants present after concurrent writes

**P1 (8 tests):**
- `test_cache_full_continuous_write` — Eviction under sustained pressure
- `test_worker_thread_pool_saturation` — Thread pool full + new notifications
- `test_miss_storm_after_purge` — PURGE all → miss storm → recovery
- `test_slow_origin_cold_cache` — 5s origin delay, concurrent requests
- `test_ipc_socket_reconnection` — Kill/restart worker 20 times
- `test_ram_disk_cache_consistency` — Write-around semantics correct
- `test_large_file_not_recorded` — 20MB files skip recording
- `test_cache_miss_cascade` — HTML → CSS → images dependency chain

**P2 (4 tests):**
- `test_hot_url_tracker_dedup` — Duplicate notifications coalesced
- `test_cache_corruption_recovery` — Corrupt bytes → detect + recover
- `test_management_socket_concurrent` — 100 threads × 1000 STATS commands
- `test_websocket_connection_churn` — Connect/disconnect 100 WS clients

### Module 9: `test_deployment_config.py` (15 tests)

Tests deployment scenarios, configuration, process management, migration.

**P0 (6 tests):**
- `test_multiple_nginx_workers` — 4 workers, no corruption
- `test_cache_permissions_cross_process` — chmod 666 works
- `test_v3_metadata_read_by_v4_binary` — Forward-compatible
- `test_worker_death_nginx_continues` — Cache HITs after worker kill
- `test_nginx_reload_under_load` — No dropped requests
- `test_cache_on_full_disk` — ENOSPC handling

**P1 (6 tests):**
- `test_worker_starts_before_nginx` — Socket exists before nginx
- `test_nginx_starts_before_worker` — Nginx retries notification
- `test_worker_graceful_shutdown` — SIGTERM drains queue
- `test_worker_catchup_missed_notifications` — Buffer while offline
- `test_mixed_v3_v4_alternates` — Both versions in same chain
- `test_socket_path_length` — Near 108-char limit

**P2 (3 tests):**
- `test_config_invalid_cache_path` — Startup validation
- `test_config_negative_max_age` — Integer validation
- `test_cache_warmup_after_eviction` — LRU repopulation

### Module 10: `test_observability.py` (12 tests)

Tests stats, metrics, health checks, event streaming.

**P0 (3 tests):**
- `test_stats_counter_accuracy` — Hit/miss counts match
- `test_prometheus_format_valid` — Parseable by Prometheus
- `test_prometheus_no_high_cardinality` — No per-URL labels

**P1 (6 tests):**
- `test_stats_concurrent_readers` — 100 threads read /stats
- `test_health_check_responsive_under_load` — <1s response
- `test_health_check_during_degraded` — Reports degraded state
- `test_websocket_event_ordering` — Causal order
- `test_stats_timing_accuracy` — Latency percentiles
- `test_stats_monotonic_counters` — Counters never decrease

**P2 (3 tests):**
- `test_websocket_late_subscriber` — No event replay
- `test_websocket_disconnect_cleanup` — No memory leak
- `test_stats_counter_overflow` — 2^32 wraps or extends

## Origin Server Implementation

### Technology: Python + FastAPI

```python
# origin/main.py
from fastapi import FastAPI, Request, Response
from fastapi.responses import StreamingResponse
import asyncio, hashlib, time

app = FastAPI()
counters = {}

@app.get("/status/{code}")
async def status_code(code: int, body: str = "", cache: int = 0):
    headers = {}
    if cache > 0:
        headers["Cache-Control"] = f"max-age={cache}"
    return Response(
        content=body.encode() if body else b"",
        status_code=code,
        headers=headers,
    )

@app.get("/cc/{directive}")
async def cache_control(directive: str, max_age: int = 0, s: int = 0, m: int = 0):
    # ... route per directive

@app.get("/chunked/{pattern}")
async def chunked(pattern: str, delay: float = 0):
    async def generate():
        if pattern == "small":
            for byte in b"x" * 1024:
                yield bytes([byte])
        elif pattern == "large":
            for _ in range(5):
                yield b"x" * (1024 * 1024)
        elif pattern == "slow":
            for _ in range(10):
                yield b"x" * 1024
                await asyncio.sleep(delay or 2)
    return StreamingResponse(generate(), media_type="text/css")

@app.get("/size/{spec}")
async def sized_response(spec: str, type: str = "text/css"):
    size = parse_size(spec)  # "10mb" → 10485760
    return Response(content=b"x" * size, media_type=type)

@app.get("/counter/{name}")
async def counter(name: str):
    counters[name] = counters.get(name, 0) + 1
    return {"count": counters[name]}
```

### Dockerfile

```dockerfile
FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY . .
HEALTHCHECK CMD curl -f http://localhost:8081/health || exit 1
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8081"]
```

## Test Client Design

### Extended ComplianceClient

```python
class ProductionTestClient:
    def __init__(self, base_url, origin_url=None):
        self.base_url = base_url
        self.origin_url = origin_url
        self.session = requests.Session()

    def get(self, path, **kwargs):
        return self.session.get(f"{self.base_url}{path}", **kwargs)

    def poll_for_hit(self, path, max_attempts=30, interval=1, **kwargs):
        for _ in range(max_attempts):
            r = self.get(path, **kwargs)
            if r.headers.get("X-PageSpeed") == "HIT":
                return r
            time.sleep(interval)
        pytest.fail(f"Never got HIT for {path}")

    def assert_miss(self, response):
        assert response.headers.get("X-PageSpeed") == "MISS"

    def assert_hit(self, response):
        assert response.headers.get("X-PageSpeed") == "HIT"

    def assert_not_cached(self, path, attempts=3, interval=2):
        for _ in range(attempts):
            r = self.get(path)
            assert r.headers.get("X-PageSpeed") == "MISS"
            time.sleep(interval)

    def verify_body_hash(self, path, expected_hash, **kwargs):
        r = self.get(path, **kwargs)
        actual = hashlib.sha256(r.content).hexdigest()
        assert actual == expected_hash, f"Body hash mismatch for {path}"
        return r

    def get_origin_counter(self, name):
        r = requests.get(f"{self.origin_url}/counter/{name}")
        return r.json()["count"]

    def reset_origin_counters(self):
        requests.post(f"{self.origin_url}/reset-counters")
```

## Execution

### Run Full Suite

```bash
cd tools/production-tests
./run.sh                          # Start stack + run all tests
./run.sh -k "test_status"        # Run specific module
./run.sh --priority p0            # P0 tests only
```

### Run Script

```bash
#!/bin/bash
set -e
cd "$(dirname "$0")"

# Build and start stack
docker compose build
docker compose up -d
sleep 5  # Wait for services

# Run tests
pytest tests/ -v --tb=short "$@"

# Collect logs on failure
if [ $? -ne 0 ]; then
    docker compose logs > test-failure-logs.txt
fi
```

### Parallel Execution

Tests are module-independent and can run with pytest-xdist:

```bash
pytest tests/ -n 4 -v  # 4 parallel workers
```

Module ordering constraints:
- `test_status_codes` can run in parallel with `test_headers_security`
- `test_stress_integrity` should run last (may affect cache state)
- `test_deployment_config` needs isolated stack (restart tests)

## File Structure

```
tools/production-tests/
├── DESIGN.md                    # This document
├── run.sh                       # Main test runner
├── docker-compose.yml           # 3-service stack
├── Dockerfile.nginx             # Nginx + pagespeed module
├── Dockerfile.worker            # Worker daemon
├── nginx.conf                   # Nginx config for tests
├── origin/
│   ├── Dockerfile               # FastAPI origin
│   ├── requirements.txt         # fastapi, uvicorn
│   ├── main.py                  # Dynamic origin server
│   └── testdata/                # Static test files
│       ├── small.jpg            # Small JPEG for image tests
│       ├── large.jpg            # Large JPEG (5MB)
│       ├── style.css            # CSS with imports
│       ├── script.js            # JS file
│       ├── page.html            # HTML with resources
│       └── animated.gif         # Multi-frame GIF
├── conftest.py                  # Shared fixtures
├── helpers/
│   ├── client.py                # ProductionTestClient
│   ├── metrics.py               # Metrics helpers (from stress)
│   └── assertions.py            # Custom assertions
└── tests/
    ├── test_status_codes.py
    ├── test_conditional_requests.py
    ├── test_cache_control.py
    ├── test_range_requests.py
    ├── test_content_negotiation.py
    ├── test_streaming_transfer.py
    ├── test_headers_security.py
    ├── test_stress_integrity.py
    ├── test_deployment_config.py
    └── test_observability.py
```

## Priority Matrix

| Priority | Count | Focus | Run Time |
|----------|-------|-------|----------|
| P0 | 79 | Data integrity, security, core HTTP | ~5 min |
| P1 | 64 | Robustness, edge cases, resilience | ~10 min |
| P2 | 32 | Rare scenarios, spec completeness | ~5 min |
| **Total** | **175** | | **~20 min** |

## Success Criteria

The suite passes when:
1. All P0 tests pass (hard gate for production)
2. All P1 tests pass (soft gate, review exceptions)
3. P2 failures documented as known limitations
4. No test flakiness (retry 3x before declaring failure)
5. Test execution < 30 minutes

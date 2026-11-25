# HTTP Compliance Test Suite

Verifies HTTP protocol correctness for the mod_pagespeed 2.1 proxy. Since
PageSpeed modifies response bodies (CSS/JS minification, image transcoding,
critical CSS injection), HTTP compliance is non-trivial: Content-Length
changes, Content-Type changes, ETags invalidate, and caching semantics
interact with body modification.

## Test Suites

### 1. Pytest (primary)

~170 tests across 14 test files covering:

| File | Category | Tests |
|------|----------|-------|
| `test_authorization.py` | RFC 9111 §3.5 Authorization gate | 7 |
| `test_content_length.py` | Content-Length after body modification | 14 |
| `test_conditional_requests.py` | If-None-Match, If-Modified-Since, 304 | 16 |
| `test_content_negotiation.py` | Accept, Vary, Content-Type | 22 |
| `test_cookie_request.py` | Cookie-bearing requests and the shared cache | 25 |
| `test_cache_control.py` | Cache-Control forwarding/respect | 16 |
| `test_transfer_encoding.py` | Chunked encoding | 10 |
| `test_head_consistency.py` | HEAD vs GET | 8 |
| `test_error_passthrough.py` | 4xx/5xx from origin | 12 |
| `test_hop_by_hop.py` | Hop-by-hop header stripping | 8 |
| `test_range_requests.py` | 206 Partial Content | 8 |
| `test_host_forwarding.py` | Host, X-Forwarded-For | 6 |
| `test_encoding.py` | Accept-Encoding, Content-Encoding | 10 |
| `test_method_handling.py` | POST/PUT/DELETE bypass | 8 |
| `test_pagespeed_specific.py` | X-PageSpeed, body integrity, magic bytes | 20 |
| `test_headers_sidecar.py` | Origin headers a cache hit cannot reproduce are not cached; cache hits carry the origin's security headers | 12 |
| `test_daemon_html.py` | Module-stored HTML through the C API (module stand-in) and `html_mode` on the front | 16 |

### 2. Hurl (declarative)

7 `.hurl` files with ~50 assertions for stateless request/response checks:

- `conditional_304.hurl` — ETag capture and conditional requests
- `cache_control_passthrough.hurl` — Cache-Control directive forwarding
- `error_passthrough.hurl` — Error status code passthrough
- `head_get_consistency.hurl` — HEAD matches GET headers
- `hop_by_hop.hurl` — Hop-by-hop header stripping
- `content_type.hurl` — Content-Type correctness
- `method_bypass.hurl` — POST/PUT/DELETE bypass

### 3. IETF cache-tests (mnot/proxy-cache-tests)

Hundreds of RFC 9111 cache behavior tests. Results comparable with
cache-tests.fyi. We maintain `known_failures.json` for expected
failures due to body modification.

## Prerequisites

- Docker and Docker Compose
- `pagespeed2-dev` image (`./tools/docker-build.sh`)
- Python 3 with pip
- hurl (optional: `brew install hurl`)

## How to Run

```bash
# All compliance tests (pytest + hurl + cache-tests)
./tools/http-compliance/run_compliance.sh

# Only pytest
./tools/http-compliance/run_compliance.sh --pytest

# Only hurl
./tools/http-compliance/run_compliance.sh --hurl

# Only cache-tests (IETF)
./tools/http-compliance/run_compliance.sh --cache-tests

# Specific test module
cd tools/http-compliance && pytest test_content_length.py -v

# Specific test by name
cd tools/http-compliance && pytest test_conditional_requests.py -v -k "etag_304"

# Run with extra pytest flags
./tools/http-compliance/run_compliance.sh -k "content_length" -v --tb=long
```

## Architecture

### Docker Compose Stack

Same pattern as E2E tests: 3 services sharing a named volume, plus a
fourth worker on its own volume for the module stand-in.

| Service | Image | Port | Purpose |
|---------|-------|------|---------|
| origin | python:3.12-slim | 8081 | Programmable compliance origin |
| worker | pagespeed2-dev build | — | Factory worker daemon |
| nginx | pagespeed2-dev build | 8080 | Nginx with pagespeed module |
| module-worker | pagespeed2-dev build | — (no network) | Worker fed by the module stand-in (`module_standin.py`) |

### Compliance Origin (`compliance_origin.py`)

A full-featured Python HTTP server (replaces the simple `http.server`
from E2E) supporting:

- Conditional requests (ETag, Last-Modified → 304)
- HEAD method (same headers, no body)
- Range requests (206 Partial Content)
- Error routes (/error/404, /error/500, etc.)
- Cache-Control test routes (/cc/no-store, /cc/max-age, etc.)
- POST/PUT/DELETE echo routes
- Chunked transfer encoding
- Hop-by-hop headers test route
- Weak ETag support

### Nginx Config

Two location blocks:
- `/` — pagespeed enabled (system under test)
- `/baseline/` — pagespeed disabled (control group)

Gzip enabled for encoding tests.

### Test Fixtures

`ComplianceClient` extends the E2E `PageSpeedClient` with:
- `assert_content_length_matches_body()` — RFC 9110 Section 8.6
- `assert_has_date()` — RFC 9110 Section 6.6.1
- `assert_valid_content_length()` — Format validation
- `head()`, `post()`, `put()`, `delete()`, `options()` — Method helpers
- `poll_for_hit()` — Wait for cache HIT (same as E2E)

Three client fixtures:
- `client` — nginx with pagespeed on
- `baseline_client` — nginx with pagespeed off (/baseline/ prefix)
- `origin_client` — direct origin access (bypass proxy)

## Adding New Tests

1. Add test data files to `testdata/`
2. Add special routes to `compliance_origin.py` if needed
3. Write tests in the appropriate `test_*.py` file
4. For stateless checks, consider adding a `.hurl` file
5. Run the full suite to verify

## Known Module Behaviors

From `ngx_pagespeed_module.cc`:

- **Content-Length**: Set from cached data size on HIT (line ~757)
- **Content-Type**: Extension-based lookup + WebP magic byte override
- **Method filter**: Only GET and HEAD processed
- **X-PageSpeed**: MISS on proxied 2xx, HIT on cache-served
- **Conditional requests**: If-None-Match not handled for cache HITs
- **AVIF detection**: Content-Type set based on file extension (may not
  detect AVIF magic bytes like it does for WebP)

## RFC References

- RFC 9110: HTTP Semantics
- RFC 9111: HTTP Caching
- RFC 9112: HTTP/1.1

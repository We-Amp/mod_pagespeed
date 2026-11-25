# HTTP Compliance Tests

## Purpose

RFC 9111 HTTP caching compliance validation for the PageSpeed nginx module. Verifies
that the proxy correctly handles Cache-Control directives, conditional requests, content
negotiation, encoding, range requests, and other HTTP protocol behaviors.

## Test Tools

Three complementary test suites, all coordinated by a single runner:

- **pytest** -- Primary suite. Python tests using `requests` against a Docker Compose stack
  (nginx + worker + programmable origin). Tests in `test_*.py` files.
- **Hurl** -- Declarative HTTP assertions in `hurl/*.hurl` files. Requires `hurl` CLI
  (`brew install hurl`). Skipped if not installed.
- **IETF cache-tests** -- Third-party cache conformance suite in `cache-tests/`. Runs via
  its own `run_cache_tests.sh` script. Skipped if not present.

## How to Run

```bash
./tools/http-compliance/run_compliance.sh              # All three suites
./tools/http-compliance/run_compliance.sh --pytest      # Only pytest
./tools/http-compliance/run_compliance.sh --hurl        # Only Hurl
./tools/http-compliance/run_compliance.sh --cache-tests # Only IETF cache-tests
./tools/http-compliance/run_compliance.sh -k no_store   # Specific pytest tests
```

The runner creates a Python venv, installs dependencies (`pytest`, `requests`, `brotli`),
and manages Docker Compose lifecycle automatically.

## Docker Dependency

Requires the `pagespeed2-dev` image. Build it first with `./tools/docker-build.sh`.
The Docker Compose stack runs three services on a shared volume:
- **origin** (:8081) -- Programmable Python HTTP server (`compliance_origin.py`)
- **worker** -- PageSpeed factory worker
- **nginx** (:8080, :8083) -- Nginx with pagespeed module (system under test)
- **module-worker** -- a second worker on its own volume with no network; the
  module stand-in (`module_standin.py`, run with `docker compose exec`) stores
  pages in it through `libpagespeed.so` the way a web-server module does

Set `COMPLIANCE_NO_LIFECYCLE=1` to skip Docker build/up/down (for CI or external management).

## What Is Tested

| File | Coverage |
|------|----------|
| `test_authorization.py` | RFC 9111 §3.5 Authorization gate: pass-through without a permit, public/s-maxage/must-revalidate permits, stale tightening, anonymous isolation |
| `test_cookie_request.py` | Cookie-bearing requests: pages (HTML, XHTML, upper-case types, JSON) stored and served only when marked public or s-maxage above zero (s-maxage=0 and malformed values are not marks); scripts without a stated lifetime not stored from a cookie request; revalidated, stale-if-error and coalesced stale serves; Authorization and Cookie together pass both rules |
| `test_cache_control.py` | no-store, no-cache, private, max-age, must-revalidate, s-maxage |
| `test_conditional_requests.py` | ETag forwarding, If-None-Match, 304 responses |
| `test_content_length.py` | Content-Length accuracy on HIT and MISS |
| `test_content_negotiation.py` | Accept-based content type selection |
| `test_encoding.py` | gzip/brotli encoding pass-through |
| `test_head_consistency.py` | HEAD vs GET header consistency |
| `test_hop_by_hop.py` | Hop-by-hop header stripping |
| `test_error_passthrough.py` | 4xx/5xx origin error forwarding |
| `test_method_handling.py` | POST/PUT/DELETE bypass caching |
| `test_range_requests.py` | Range request handling |
| `test_transfer_encoding.py` | Transfer-Encoding correctness |
| `test_host_forwarding.py` | Host header forwarding to origin |
| `test_pagespeed_specific.py` | PageSpeed-specific behaviors (X-PageSpeed header, etc.) |
| `test_headers_sidecar.py` | Response-header sidecar: unreproducible origin headers / nonce CSP / Vary: Origin never cached; HIT, coalesced stale, stale-if-error and 304-revalidated serves carry the MISS's security headers; a removed stored block (`seed_test_cache --remove_alternate 0x6c` in the nginx container) is a MISS then re-recorded; counters in the shared serve-stats file |
| `test_daemon_html.py` | Pages a web-server module stores itself: a stand-in stores the page, its paired header block and its stamp through the C API in one site and notifies `module-worker`; the Markdown copy is derived with no network and no HTML variant in both `markdown-*` modes, is bound to the stamp (entity tag, serve verdict) and unbound by a changed page; below-floor and hash-mismatch pages are withheld; unvouched shapes are refused by the library, and `full`/`off`, non-HTML-store, `no-transform` and missing pages are refused and counted by the worker. On the nginx front, `disable_html=false` is `full` and `markdown-static` runs no HTML pass |

## Adding New Tests

1. Create `test_<category>.py` or add to an existing file
2. Use the `client` fixture (nginx), `origin_client` (direct origin), or `aggressive_client`
3. Add test data files to `testdata/` and origin routes to `compliance_origin.py`
4. For Hurl tests, add `.hurl` files to `hurl/`

## When to Run

Run after any change to HIT-path Cache-Control logic in `src/nginx/`, cache header
handling in `src/worker/`, or proxy behavior in the nginx configuration.

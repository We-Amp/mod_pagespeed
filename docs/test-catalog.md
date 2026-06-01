# Test Catalog

Comprehensive reference of test baselines, skip markers, and platform-specific behavior across all deployment modes.

Last updated: 2026-02-10

## Test Suite Baselines

### C++ Unit Tests

```
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/... //test/net/...
```

| Status | Count | Notes |
|--------|-------|-------|
| Passed | 45 | |
| Failed (build) | 1 | `iis_html_flushing_integration_test` — pre-existing `TakePendingChunks` missing |
| Skipped | 15 | IIS tests (require Windows) |
| Network-dependent | 1 | `curl_url_async_fetcher_test` — tagged `requires-network` |

### Apache System Tests

```
./test/system/run_system_tests.sh
```

| Status | Count | Notes |
|--------|-------|-------|
| Passed | 271 | After test fixes (was 191 with 4 failures) |
| Skipped | 224 | Envoy/nginx-only, HTTPS, IIS, secondary host |
| Failed (pre-existing) | 5 | See [Apache Known Failures](#apache-known-failures) |
| xfailed | 1 | |

### Envoy System Tests

```
./test/system/run_envoy_tests.sh
```

| Status | Count | Notes |
|--------|-------|-------|
| Passed | 175 | |
| Skipped | 60 | See [Envoy Skips](#envoy-skips) |
| xpassed | 1 | `test_pagespeed_header_present` — marked xfail but now passes |
| Failed | 0 | |

### Nginx System Tests

```
./test/system/run_nginx_tests.sh
```

| Status | Count | Notes |
|--------|-------|-------|
| Passed | 200 | |
| Skipped | 72 | Shared `not_envoy`/`not_nginx` + nginx-specific skips |
| Failed | 0 | |

---

## Apache Known Failures

These failures are pre-existing and not regressions:

| Test | File | Issue |
|------|------|-------|
| `test_resources_not_private_cache` | test_headers.py | Intermittent — depends on downstream cache config adding `Cache-Control: private` to HTML responses which propagates to resource headers |
| `test_outlined_js_has_last_modified` | test_outliners.py | 404 on outlined JS resource — outlined resource not found |
| `test_combine_css_basic` | test_example.py | CSS combination timeout — async optimization too slow |
| `test_combine_css_count` | test_example.py | CSS combination timeout — same root cause |
| `test_cache_control_headers` | test_example.py | Depends on combine_css which times out |

---

## Envoy Skips (~60 tests)

### Streaming Architecture Limitations

Tests skipped because Envoy uses non-blocking streaming ProxyFetch that doesn't support `X-PSA-Blocking-Rewrite`:

| Category | Tests | Marker | Reason |
|----------|-------|--------|--------|
| Local Storage Cache | ~8 | `not_envoy` | Requires blocking rewrite for deterministic results |
| Flatten CSS Imports | ~4 | `not_envoy` | Requires blocking rewrite |
| Lazyload Images | ~8 | `not_envoy` | Filter times out in streaming architecture |
| Outliners (CSS + JS) | ~7 | `not_envoy` | Requires blocking rewrite |
| WebP Optimization | ~4 | `not_envoy` | Requires blocking rewrite |
| Defer Images | ~2 | `not_envoy` | Requires blocking rewrite |
| Combine CSS (test_example.py) | ~3 | `not_envoy` | CSS combination times out |
| Blocking Rewrite Stats | 1 | `not_envoy` | Requires blocking rewrite |

### Filter Timeouts

| Category | Tests | Reason |
|----------|-------|--------|
| Inline Preview Images | ~3 | Filter times out in streaming architecture |
| CSS Sprite External | ~1 | CSS sprite generation times out |
| Canonicalize JS | ~1 | Filter times out |

### Feature Gaps

| Category | Tests | Reason |
|----------|-------|--------|
| IPRO Short Cache Lifetime | 1 | Returns `max-age=3598` instead of `<1000` — different cache timing |
| Query Params Header Disable | 1 | `PageSpeedFilters` header not respected for IPRO resource requests |
| Resource 404 Count | 1 | Envoy doesn't track `resource_404_count` statistic |
| IPRO ETag Format | ~2 | Doesn't produce `PSA-aj` ETag pattern like Apache |
| Rel Canonical IPRO | ~2 | IPRO doesn't produce `PSA-aj` ETag pattern |
| data-pagespeed-no-transform | 1 | Image rewriting filter behavior differs |

### HTTPS Conditional Skips

| Category | Tests | Reason |
|----------|-------|--------|
| HTTPS CSS Combination | 3 | `test_https.py` — CSS combination times out over HTTPS |

### Envoy xpassed (1 test)

| Test | File | Notes |
|------|------|-------|
| `test_pagespeed_header_present` | envoy/test_envoy_sanity.py | Marked `xfail` (X-Page-Speed header crashes during local reply) but now passes. Consider removing the xfail marker. |

---

## Nginx Skips

Nginx shares the same streaming architecture limitations as Envoy, plus additional nginx-specific issues.

### Shared with Envoy (all `not_envoy` tests are also `not_nginx`)

All tests marked `not_envoy` above are also marked `not_nginx`.

### Nginx-Only Skips

| Category | Tests | File | Reason |
|----------|-------|------|--------|
| Content-Length | ~2 | test_content_length.py | Nginx uses chunked encoding and adds `Cache-Control: private` |
| No-Cache Resources | ~2 | test_no_cache.py | Nginx IPRO doesn't preserve no-cache headers |
| Rewritten Image ETag | 1 | test_rewrite_images.py | Nginx doesn't add ETag to `.pagespeed.` resources |

---

## IIS-Only Tests

Tests that only run when `PAGESPEED_SERVER_TYPE=iis`:

| File | Tests | Category |
|------|-------|----------|
| test_iis_sanity.py | 10 | Basic connectivity, headers (1 xfail: heap corruption) |
| test_iis_admin.py | 26 | Admin UI, health checks |
| test_iis_config.py | 21 | web.config parsing |
| test_iis_statistics.py | 25 | Statistics endpoints |
| test_iis_error_handling.py | 18 | Error recovery |
| test_iis_headers.py | 19 | HTTP response headers |
| test_iis_beacons.py | 13 | Beacon handling |
| test_iis_cache.py | 15 | Caching behavior |
| test_iis_blocking_rewrite.py | 11 | Blocking rewrite |

**Total: ~158 IIS-specific tests**

IIS Python integration tests (run on Windows): 350/356 passed, 5 skipped, 1 timing-dependent.

---

## HTTPS Tests

All tests in `test/system/automatic/test_https.py` require `PAGESPEED_HTTPS_HOST` environment variable. When not set, the entire module is skipped.

| Test | Envoy Status | Apache Status |
|------|-------------|---------------|
| `test_https_basic_rewriting` | Pass | Pass |
| `test_https_css_combination` | Skip (timeout) | Pass |
| `test_https_combined_css_with_filters` | Skip (timeout) | Pass |

---

## Test Infrastructure Notes

### Pytest Markers (defined in conftest.py)

| Marker | Behavior |
|--------|----------|
| `not_envoy` | Skip when `PAGESPEED_SERVER_TYPE=envoy` |
| `not_nginx` | Skip when `PAGESPEED_SERVER_TYPE=nginx` |
| `not_iis` | Skip when `PAGESPEED_SERVER_TYPE=iis` |
| `not_apache` | Skip when `PAGESPEED_SERVER_TYPE=apache` (currently unused) |
| `apache_only` | Only run on Apache |
| `envoy_only` | Only run on Envoy |
| `nginx_only` | Only run on Nginx |
| `iis_only` | Only run on IIS |
| `requires_secondary` | Requires `PAGESPEED_SECONDARY_HOST` |
| `requires_https` | Requires `PAGESPEED_HTTPS_HOST` |
| `requires_stats` | Requires statistics enabled |
| `requires_module` | Requires PageSpeed module installed |
| `slow` | Long-running test |

### Test Discovery

Tests are organized in `test/system/`:
- `automatic/` — Golden standard tests shared across all platforms
- `system/` — System-level tests (IPRO, headers)
- `envoy/` — Envoy-specific tests
- `nginx/` — Nginx-specific tests (TBD)
- `iis/` — IIS-specific tests (run on Windows)

### Common Root Causes for Skips

1. **`X-PSA-Blocking-Rewrite` not supported**: Envoy and Nginx use streaming non-blocking architecture. Tests that need deterministic rewriting results must use `fetch_until` polling instead.

2. **CSS combination timeouts**: The `combine_css` filter requires async worker pool coordination that doesn't complete in time on streaming architectures.

3. **Animated GIF to WebP conversion**: The `convert_to_webp_animated` filter is computationally expensive and may not complete in the non-blocking rewriting window. Requires blocking rewrite and `Accept: image/webp` header.

4. **IPRO behavioral differences**: Envoy/Nginx IPRO returns different cache lifetimes (`max-age` minus elapsed time vs. `implicit_cache_ttl_ms`) and different ETag formats.

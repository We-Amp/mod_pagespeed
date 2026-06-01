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
| Passed | 195 | |
| Skipped | 14 | HTTPS, IIS, secondary host |
| Failed | 0 | |

### Envoy System Tests

```
./test/system/run_envoy_tests.sh
```

| Status | Count | Notes |
|--------|-------|-------|
| Passed | 189 | |
| Skipped | 20 | See [Envoy Skips](#envoy-skips) |
| Failed | 0 | |

### Nginx System Tests

```
./test/system/run_nginx_tests.sh
```

| Status | Count | Notes |
|--------|-------|-------|
| Passed | 169 | |
| Skipped | 40 | Shared `not_envoy`/`not_nginx` + nginx-specific skips |
| Failed | 0 | |

---

---

## Envoy Skips (~20 tests)

Most streaming architecture limitations (X-PSA-Blocking-Rewrite) were resolved by converting tests
to use `fetch_until_contains`/`fetch_until_count` polling instead of blocking rewrite headers.

### Remaining Envoy Skips

| Category | Tests | Marker | Reason |
|----------|-------|--------|--------|
| Combine CSS (test_example.py) | ~3 | `not_envoy` | CSS combination times out |
| Blocking Rewrite Stats | 1 | `not_envoy` | Tests blocking rewrite concept itself |
| CssFlattenMaxBytes header | 1 | `not_envoy` | Header handling differs |
| Query Params Header Disable | 1 | `not_envoy` | `PageSpeedFilters` header not respected for IPRO |
| IPRO Short Cache Lifetime | 1 | `not_envoy` | Different cache timing behavior |
| Resource 404 Count | 1 | `not_envoy` | Envoy doesn't track `resource_404_count` statistic |
| IPRO ETag Format | ~2 | `not_envoy` | Doesn't produce `PSA-aj` ETag pattern like Apache |
| Rel Canonical IPRO | ~2 | `not_envoy` | IPRO doesn't produce `PSA-aj` ETag pattern |
| HTTPS CSS Combination | 3 | `not_envoy` | `test_https.py` — CSS combination times out over HTTPS |

---

## Nginx Skips (~26 tests)

Nginx shares all Envoy skip markers, plus additional nginx-specific issues.

### Shared with Envoy (all `not_envoy` tests are also `not_nginx`)

All tests marked `not_envoy` above are also marked `not_nginx`.

### Nginx-Only Skips

| Category | Tests | File | Reason |
|----------|-------|------|--------|
| ~~No-Cache Resources~~ | ~~2~~ | ~~test_no_cache.py~~ | Fixed: nginx config now sets Cache-Control: no-cache for /no_cache/ directory |

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

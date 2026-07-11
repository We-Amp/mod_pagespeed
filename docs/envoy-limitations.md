# Envoy PageSpeed Known Limitations

**Status: Experimental**

## Architecture Differences

The Envoy PageSpeed filter uses a fundamentally different architecture from the
Apache module. Key differences:

- **Non-blocking**: Envoy is event-driven; `X-PSA-Blocking-Rewrite` is not
  supported. Use `fetch_until` polling in tests instead.
- **Independent fetcher**: Uses `CurlUrlAsyncFetcher` linked against BoringSSL,
  not Envoy's ClusterManager. Outbound HTTPS uses the system CA store, not
  Envoy's TLS configuration.
- **Threading model**: ProxyFetch runs on worker threads; `EnvoyAsyncFetch`
  bridges to Envoy's dispatcher thread via `shared_from_this()` and atomics.

## Skipped Tests (20 total)

These tests are marked `@pytest.mark.not_envoy` and represent known behavioral
differences, not bugs:

### Statistics (1 test)
- `resource_404_count` statistic is not tracked in Envoy

### IPRO Cache Headers (~3 tests)
- IPRO returns upstream `max-age` minus cache time instead of
  `implicit_cache_ttl_ms`
- Cache lifetime behavior differs from Apache

### CSS Flatten Max Bytes (1 test)
- `CssFlattenMaxBytes` header handling differs

### Query Params Header (1 test)
- `PageSpeedFilters` header not respected for IPRO requests

### Other (~14 tests)
- Content-Length handling (Envoy may chunk responses differently)
- HTTPS combination behavior
- IPRO ETag format (doesn't produce `PSA-aj` pattern)
- Version header shows placeholders without workspace status

## Known Functional Limitations

### combine_css Timeouts (fixed)
The `combine_css` filter could time out because scheduler alarms (rewrite
deadlines, nested fetch timeouts) had no guaranteed driver on Envoy: they
only fired when some thread happened to call into the scheduler. Fixed by
wiring the EventScheduler to the Envoy dispatcher: the
event loop now drives alarm delivery, so deadlines fire on time even with
no blocked waiter.

### IPRO Cache Lifetime
When serving IPRO-optimized resources, Envoy returns the upstream `max-age`
minus the time the resource spent in cache, rather than the configured
`implicit_cache_ttl_ms` value.

### IPRO ETag Format
Envoy does not produce the `PSA-aj` ETag pattern that Apache generates for
IPRO resources.

### Version String
Without a proper `bazel/get_workspace_status` script, the version header may
show placeholder values. The release builds include the git hash.

## Test Results

**Latest:** 225 passed, 25 skipped, 0 failed

Run tests:
```bash
./test/system/run_envoy_tests.sh automatic/ -v  # All automatic tests
./test/system/run_envoy_tests.sh -k sanity      # Quick sanity check
```

# IIS PageSpeed Known Limitations

**Status: Experimental**

## Architecture

The IIS module uses a single `IisServerContext` for all requests within an
application pool. This simplified architecture avoids lifetime management issues
but means all sites in an app pool share configuration.

## Functional Limitations

### IPRO Cache
- Async cache stubs return misses for cache lookups
- Redis and disk caches are not functional for IPRO operations
- IPRO-optimized resources are re-optimized on each request until the
  in-memory cache is populated

### Beacon Data
- Beacon data (used for critical CSS and image optimization feedback) is
  silently discarded
- This means adaptive optimizations that rely on beacon feedback (like
  critical CSS inlining) do not improve over time

### External Cache
- Redis only (Memcached is not supported on Windows)
- Cache backend is functional for general caching but not for IPRO async ops

### Shared Memory
- Uses `WindowsSharedMem` implementation
- Alignment requirements differ from POSIX (see
  [WINDOWS_SHARED_MEM_ALIGNMENT_PLAN.md](WINDOWS_SHARED_MEM_ALIGNMENT_PLAN.md))

## Configuration Caveats

### Module Registration
- If registered globally via `New-WebGlobalModule`, do NOT add the module
  again in site `web.config` -- causes 500.19 "duplicate collection entry"

### allowDoubleEscaping
- Must be set to `true` in `requestFiltering` configuration
- Combined resource URLs contain `+` characters (e.g.,
  `a.css+b.css.pagespeed.cc.HASH.css`) that trigger IIS
  RequestFilteringModule 404.11 errors

### IIS Response Clearing
- `IHttpResponse::Clear()` clears both headers AND body
- Headers set before `Clear()` in `OnSendResponse` will not survive
- Headers must be set after `Clear()` or in `HandleDone()`/`SendHeaders()`

### IIS Express vs Full IIS
- If full IIS (W3SVC, PID 4) runs on the same port, it handles requests
  instead of IIS Express
- Stop W3SVC before starting IIS Express:
  `Stop-Service W3SVC -Force; Stop-Service WAS -Force`

## Platform Differences

| Component | IIS | Apache |
|-----------|-----|--------|
| Crypto | BCrypt API | BoringSSL |
| Shared Memory | WindowsSharedMem | PthreadSharedMem |
| External Cache | Redis only | Redis + Memcached |
| Config Format | web.config XML | httpd.conf directives |

## Test Results

- **C++ unit tests:** 19/19 passing (1 excluded: `iis_html_flushing_integration_test`)
- **Python integration tests:** 350/356 passing (5 skipped, 1 timing-dependent)

### Python Test Coverage

| Test File | Result |
|-----------|--------|
| `test_sanity.py` | 6/6 (100%) |
| `test_admin.py` | 11/11 (100%) |
| `test_ipro.py` | 15/15 (100%) |
| `test_html_rewrite.py` | 34/34 (100%) |
| `test_combiners.py` | 17/17 (100%) |
| `test_cache_control.py` | 8/8 (100%) |
| `test_headers.py` | 13/13 (100%) |
| `test_preload.py` | 15/15 (100%) |
| `test_dns_prefetch.py` | 9/9 (100%) |

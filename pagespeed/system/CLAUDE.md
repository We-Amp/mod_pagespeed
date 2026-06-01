# pagespeed/system/

System-level abstractions shared by Apache and Nginx (and by Envoy).
NOT used by the IIS module, which has its own platform layer in `pagespeed/iis/`.
Provides cache management, admin UI, URL fetching, and the base factory that
Apache and Nginx factories inherit from.

## Key Components

### SystemRewriteDriverFactory (`system_rewrite_driver_factory.cc`)

Base class for `ApacheRewriteDriverFactory` and `NginxRewriteDriverFactory`.
Manages shared-memory statistics, worker thread pools, fetcher allocation,
`RootInit`/`ChildInit` lifecycle (Apache pre-fork model), and the central
controller process (gRPC-based optimization coordination).

### Admin Console (`admin_site.h`, `admin_site.cc`)

Serves the `/pagespeed_admin/` pages. All handlers return JSON; the SPA
console is a single embedded HTML page generated from Vite build output
(`admin_console.html`). Endpoints include statistics, message history,
cache inspection, configuration dump, histograms, graphs, and cache purge.

### Cache Path Management (`system_cache_path.cc`)

`SystemCachePath` encapsulates per-virtual-host cache sharing. Each file
cache path gets its own `CycloneCache` disk backend (with CLFUS eviction),
an optional `SharedMemLockManager` for inter-process coordination, and a
`PurgeContext` for `cache.flush`/`cache.purge` file watching. Falls back
to `ThreadSafeLockManager` (in-process only) when shared memory is
unavailable.

### LoopbackRouteFetcher (`loopback_route_fetcher.h`)

Routes resource fetches for hosts not in the `DomainLawyer` back to the
server's own IP and port. Essential for IPRO (In-Place Resource
Optimization) -- when PageSpeed needs to fetch a resource from the same
server, this fetcher ensures the request goes to the local backend rather
than making an external DNS-resolved request. Delegates to
`CurlUrlAsyncFetcher` as the backend.

### Other Notable Files

| File | Purpose |
|------|---------|
| `system_caches.cc` | Wires up LRU, file, Redis, Memcached, and shared-memory metadata caches |
| `curl_url_async_fetcher.cc` | libcurl-based async HTTP fetcher (used by Envoy and Apache/Nginx) |
| `circuit_breaker.cc` | Failure-counting circuit breaker for fetch resilience |
| `system_server_context.cc` | Base server context with `ChildInit`, stats, cache flush |
| `in_place_resource_recorder.cc` | Records IPRO responses into the cache |
| `system_thread_system.h` | Thread system with deferred thread-start support (for fork safety) |
| `controller_manager.cc` | Forks and manages the central controller process |

## Testing

```bash
bazel test --config=clang-libstdcxx13 //test/pagespeed/system/...
```

Some tests require Redis and/or Memcached:
```bash
bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/system/...
```

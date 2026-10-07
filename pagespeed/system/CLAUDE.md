# pagespeed/system/

System-level abstractions shared by Apache and Nginx (and by Envoy).
NOT used by the IIS module, which has its own platform layer in `pagespeed/iis/`.
Provides cache management, admin UI, URL fetching, and the base factory that
Apache and Nginx factories inherit from.

## Key Components

### SystemRewriteDriverFactory (`system_rewrite_driver_factory.cc`)

Base class for `ApacheRewriteDriverFactory` and `NginxRewriteDriverFactory`.
Manages shared-memory statistics, worker thread pools, fetcher allocation,
and `RootInit`/`ChildInit` lifecycle (Apache pre-fork model).

### Admin Console (`admin_site.h`, `admin_site.cc`)

Serves the `/pagespeed_admin/` pages. All handlers return JSON; the SPA
console is a single embedded HTML page generated from Vite build output
(`admin_console.html`). Endpoints include statistics, message history,
cache inspection, configuration dump, histograms, graphs, and cache purge.

`message_history` answers the process-wide message buffer after an optional
`since=<line cursor>` (the previous answer's `next`). With `grouped=1` it
folds the lines into `{level, template, count, last_ms[, recent]}` rows,
newest first: `MessageTemplate` (`admin_site.cc`) turns URLs into `URL`,
hexadecimal identifiers into `ID` and digit runs into `N`; the console's
`message-template.ts` implements the same rules, and both are tested against
`test/pagespeed/system/testdata/message_templates.tsv` -- change the rules in
both places and in that file together. `window_s=<1..86400>` adds `recent`,
the lines stamped within that many seconds of the server's clock (`now_ms`).
The grouped mode is GET/HEAD only, admits one read per admin site at a time
(429 `busy`) and is capped at 1 MiB (`"truncated":true`); the plain mode is
unchanged.

The `/v1/daemon/*` endpoints (`admin_daemon_handler.cc`) are a read-only
proxy to the optimizer daemon's management API: GET/HEAD only, upstream
paths from a compile-time table (a leaf is the one or two path segments
after the final `v1/daemon`), query parameters only through the table's
per-endpoint allow-list (every value validated and re-encoded
module-side; the raw client query string never reaches the daemon), a
fixed number of in-flight requests per endpoint (one; two on
`cache/content`, counted process-wide across every virtual host's
handler), 502 when the daemon is unreachable, and every response
`no-store`, `nosniff` and `Cross-Origin-Resource-Policy: same-origin`. The
cache leaves (`cache/urls`, `cache/alternates`, `cache/content`) answer 403 `whole_server_console_only` on a
per-virtual-host console -- the daemon's cache is shared across hosts; a
cached entry is named by `url` (path + query), `hostname` and `scheme`,
and a URL the daemon holds nothing for answers 404 `not_in_index`. The
`logs` leaf (the optimizer's recent log, `since`/`limit` cursors) is
whole-server only as well, and answers a read over the JSON cap with 502
`response_too_large` rather than `daemon_unreachable`. On a per-virtual-host console the `stats` and `cooldowns` leaves are narrowed by the module (`daemon_site_filter.cc`, the table's `site_filter` column): only the row of `serve_savings_by_host` named by the request's own site (`VouchedServeHost`, passed in as `AdminPage`'s `own_serve_host`; IIS and Envoy pass none, so their per-host consoles see no row) is kept and every other row is folded into `other`, and the rebuilt block is marked `"site"` (that host, `""` when none; the console uses it to pick the row), and `cooldowns` keeps only the entries whose `hostname` is that host (same host-name rule; each entry with only its known fields, typed as the optimizer writes them; `count` recomputed; of the other top-level keys only a boolean `enabled` is kept; entries are matched on the lookup host each entry was recorded for, not on the serving site's configuration, so a per-host list can include entries the site did not serve itself, never another site's own entries); an answer that is not a JSON object is 502 `daemon_unreachable`, an upstream error status is sent with `{"error":"daemon_error"}`; the whole-server console's answers are forwarded unchanged.
`cache/content` serves bytes only for image/png, image/jpeg, image/gif,
image/webp and image/avif on an upstream 200, and every one of its
responses carries a sandboxing CSP. The transport sits behind the
`DaemonReader` seam (`daemon_reader.h`), which takes a per-read response
cap (`kMaxJsonResponseBytes` 1 MiB, `kMaxContentResponseBytes` 16 MiB) and
reports why a read failed (`DaemonReadFailure`); the only implementation
is `UdsDaemonReader` (in the `curl_fetcher` target), which fetches over
the daemon's unix socket via `CurlUrlAsyncFetcher::FetchOverUnixSocket`
with a 5s timeout. Ports create the reader through the
`SystemServerContext::NewDaemonReader()` hook; the socket path comes from
the `DaemonApiSocketPath` option (Apache/nginx/Envoy; empty disables).

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
| `serve_host_names.cc` | The host a serve is recorded under in the optimizer's serve savings per host (`VouchedServeHost`): the request's host when it is one of the site's exact configured names, else the site's primary name, else none. Ports supply the names per request from the request's own configuration record (Apache: `ApacheConfiguredHostNames(request->server, main server, stated records)` -- a primary name counts only when the parsed configuration states a `ServerName` for that record, computed once per configuration in the post-config hook (`ApacheStatedServerNamesFromTree`); a name httpd derived, an inherited main-server name or an httpd placeholder is no name; nginx: `NgxConfiguredHostNames(r)`, the server block's server_name and its server_names entries except regular expressions). IIS supplies none yet (its sites' bindings are not read), so its serves carry no host; Envoy records no serves. |
| `daemon_site_filter.cc` | What a per-host admin console may see of the optimizer's answers: a strict jsoncpp reading (no repeated keys, bounded nesting), only the console's own site's row of `serve_savings_by_host`, 64-bit sums checked; fails closed |
| `system_thread_system.h` | Thread system with deferred thread-start support (for fork safety) |

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

# pagespeed/nginx/ -- Nginx Module

Nginx module implementing PageSpeed optimization as a native nginx filter module.

## Key Files

| File | Purpose |
|------|---------|
| `ngx_pagespeed.cc` | Main module: config directives, content/header filters, request routing, IPRO |
| `ngx_event_connection.cc/.h` | Pipe-based IPC: sends events from PSOL threads to nginx event loop |
| `ngx_base_fetch.h` | `NgxBaseFetch`: buffers PSOL output, signals nginx via pipe, ref-counted lifecycle |
| `ngx_base_fetch.cc` | `NgxBaseFetch` implementation: `HandleDone`, `HandleFlush`, `CollectAccumulatedWrites`, pipe signaling |
| `ngx_url_async_fetcher.cc` | `NgxUrlAsyncFetcher`: native nginx-based HTTP fetcher (alternative to curl) |
| `ngx_rewrite_driver_factory.cc` | Factory: creates server contexts, allocates curl or native fetcher |
| `ngx_server_context.cc` | Per-server state, statistics initialization |
| `ngx_rewrite_options.cc` | Nginx-specific config option parsing (`pagespeed` directive) |
| `ngx_fetch.cc` | `NgxFetch`: individual fetch request implementation for native fetcher |
| `ngx_message_handler.cc` | Logging bridge to `ngx_log_error` |

## Pipe-Based IPC (NgxEventConnection)

PSOL worker threads cannot call nginx APIs directly. Communication uses a Unix pipe:

- **`pipe_write_fd_`** -- Written by PSOL threads (any thread) to signal events
- **`pipe_read_fd_`** -- Registered with nginx event loop via `ngx_add_channel_event`
- **`ps_event_data`** -- Struct written atomically: `{type, sender, connection}`
- **`ReadEventHandler`** -- nginx calls this when pipe becomes readable; dispatches to callback

A full pipe never blocks or drops: writes that hit `EAGAIN` divert to a mutex-guarded overflow queue that the reader drains in order (see `ngx_event_connection.cc`). Pipe capacity is bumped via `F_SETPIPE_SZ` where available.

Two pipe instances exist: one for `NgxBaseFetch` (response delivery) and one per `NgxUrlAsyncFetcher` (fetch completion notification).

## Threading Rules

**CRITICAL: Never call `ngx_http_*` functions from PSOL worker threads.**

- PSOL rewrite workers run on their own thread pool
- All nginx API calls must happen on the nginx event loop thread
- `NgxBaseFetch::HandleDone/HandleFlush/HandleHeadersComplete` write to the pipe
- `NgxBaseFetch::ReadCallback` runs on the nginx thread and calls `CollectAccumulatedWrites`
- Ref-counting (`references_`) prevents use-after-free when events are in-flight

## Build

```bash
# Inside Docker dev container
bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so
```

## Test

```bash
# System tests (see docs/test-catalog.md for current pass/skip counts)
./test/system/run_nginx_tests.sh

# Run specific test
./test/system/run_nginx_tests.sh -k sanity
```

## Known Issues

- **`combine_css` timeout**: When ProxyFetch needs to fetch sub-resources before emitting output, and the fetch takes longer than nginx `send_timeout`, nginx kills the connection. Pre-fetching sub-resources before streaming is the fix pattern.
- **Streaming architecture**: nginx streams response chunks through filters. ProxyFetch may need to buffer the entire response for certain optimizations, creating back-pressure.
- **Native fetcher needs a resolver**: `UseNativeFetcher on` hard-fails at startup unless nginx.conf configures the core `resolver` directive.

## Fetcher

Two fetcher options controlled by `use_native_fetcher_`:
- **Default (curl)**: `CurlUrlAsyncFetcher` -- independent of nginx (one poll thread per worker), TLS via the module's statically linked library
- **Native**: `NgxUrlAsyncFetcher` -- runs on the nginx event loop (no fetch threads), uses nginx's resolver and connection pool, signals completion via its own `NgxEventConnection` pipe. TLS uses nginx's own SSL machinery (`ngx_ssl_*`): the handshake runs on the event loop, certificate verification mirrors the curl fetcher's `FetchHttps` semantics (chain verification skipped under `allow_self_signed`/`allow_unknown_certificate_authority`, hostname always checked), and TLS connections are keepalive-pooled per (address, host). Direct OpenSSL calls go through `ngx_openssl_shim.{h,cc}` -- module code must NOT call `SSL_*`/`X509_*` directly, those bind to the module's hidden statically linked TLS library instead of nginx's (ABI mix on nginx-owned objects). Https-through-fetch-proxy (CONNECT) is not supported: configure the curl fetcher for that.

Test harness: `PAGESPEED_TEST_NATIVE_FETCHER=1 ./test/system/run_nginx_tests.sh` runs the suite with the native fetcher (injects `resolver` + `UseNativeFetcher on`).

## Cross-References

See root `CLAUDE.md` for:
- Nginx system test status and known differences from Apache
- Nginx thread model and pipe-based IPC details
- `combine_css` timeout explanation
- Build commands and Docker development workflow

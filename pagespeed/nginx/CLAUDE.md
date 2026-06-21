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

Write retries use exponential backoff (100us to 100ms, 50 attempts max) to handle full pipe buffers. Pipe capacity is bumped via `F_SETPIPE_SZ` where available.

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
- **Native fetcher vs curl**: `NgxUrlAsyncFetcher` uses nginx's own connection pool; curl fetcher (`CurlUrlAsyncFetcher`) is the default and more reliable for HTTPS.

## Fetcher

Two fetcher options controlled by `use_native_fetcher_`:
- **Default (curl)**: `CurlUrlAsyncFetcher` -- independent of nginx, handles HTTPS via BoringSSL
- **Native**: `NgxUrlAsyncFetcher` -- uses nginx's resolver and connection pool, signals completion via its own `NgxEventConnection` pipe

## Cross-References

See root `CLAUDE.md` for:
- Nginx system test status and known differences from Apache
- Nginx thread model and pipe-based IPC details
- `combine_css` timeout explanation
- Build commands and Docker development workflow

# pagespeed/apache/ -- Apache Module (mod_pagespeed)

Apache HTTPD module implementing PageSpeed optimization. This is the most mature integration and serves as the reference implementation for other server modules.

## Key Files

| File | Purpose |
|------|---------|
| `mod_instaweb.cc` | Main entry: hook registration, config directive parsing, output filter chain |
| `apache_config.cc` | `ApacheConfig`: Apache-specific options (ProxyAuth, ForceBuffering, MeasurementProxy) |
| `apache_fetch.cc` | `ApacheFetch`: async fetch with buffered/streaming modes, scheduler-based Wait() |
| `apache_server_context.cc` | Per-vhost state, proxy fetch factory, measurement proxy support |
| `apache_rewrite_driver_factory.cc` | Factory: curl fetcher, APR timer, pthread shared memory, libevent dispatcher |
| `apache_logging_includes.h` | Safe inclusion of Apache `http_log.h` (undefines `HAVE_SYSLOG` to prevent LOG macro conflicts) |
| `instaweb_handler.cc` | Content handler for `.pagespeed.` resource URLs, admin/statistics endpoints |
| `instaweb_context.cc` | Per-request rewrite context: output filter state machine |
| `header_util.cc` | Apache-specific header manipulation utilities |
| `apache_writer.cc` | Writes rewritten content to Apache's output bucket brigade |

## Apache Hook Integration

Registered in `mod_pagespeed_register_hooks()`:

| Hook | Priority | Purpose |
|------|----------|---------|
| `ap_hook_handler` | `APR_HOOK_FIRST - 1` | Handle `.pagespeed.` resource URLs |
| `ap_hook_post_read_request` | `APR_HOOK_FIRST` | Modify request (IP forwarding) |
| `ap_hook_translate_name` | `APR_HOOK_FIRST - 2` | Save URL before mod_rewrite corrupts it |
| `ap_hook_map_to_storage` | `APR_HOOK_FIRST - 2` | Bypass 256-char filename limit for pagespeed URLs |
| `ap_hook_post_config` | `APR_HOOK_MIDDLE` | Server startup configuration |
| `ap_hook_child_init` | `APR_HOOK_LAST` | Worker process initialization |
| `ap_hook_log_transaction` | `APR_HOOK_LAST` | Request logging |

**Output filters** (registered via `ap_register_output_filter`):
- `AP_FTYPE_RESOURCE + 1` -- Main rewrite filter (runs after mod_include)
- `AP_FTYPE_CONTENT_SET + 1` -- Fix headers filter (runs after mod_headers/mod_expires)
- `AP_FTYPE_CONTENT_SET - 1` -- IPRO recording filter (before mod_deflate)
- `AP_FTYPE_PROTOCOL + 1` -- IPRO header check filter

## How Apache Differs from IIS/Envoy/Nginx

- **Most mature**: reference implementation, largest test suite (195 passed, 14 skipped)
- **Synchronous filter model**: Apache bucket brigades allow buffered or streaming operation
- **APR memory pools**: strings allocated from request pool, freed on request completion
- **Shared memory**: real cross-process shared memory via `PthreadSharedMem` (unlike IIS which uses `InProcessSharedMem`)
- **Curl fetcher**: uses `CurlUrlAsyncFetcher` with BoringSSL (same as Envoy/nginx default)

## ProcessContext Lifecycle

`ApacheProcessContext` is a file-static global constructed at module load time (not under a loader lock, unlike IIS's `DllMain`). It:
1. Calls `ApacheRewriteDriverFactory::Initialize()` in constructor
2. Creates factory lazily on first `server_rec` access
3. Destroys factory before `ProcessContext` dtor (protobuf shutdown ordering)
4. Calls `ApacheRewriteDriverFactory::Terminate()` in destructor

Apache initializes twice (config check + real startup). The factory is destroyed between runs via `apr_pool_cleanup` on the `pagespeed_child_exit` callback.

## Build and Test

```bash
# Inside Docker dev container
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so

# System tests (195 passed, 14 skipped)
./test/system/run_system_tests.sh
./test/system/run_system_tests.sh -k sanity
```

## Gotcha: LOG Macro Conflicts

`apache_logging_includes.h` undefines `HAVE_SYSLOG` before including Apache's `http_log.h`. Without this, `syslog.h` defines `LOG_INFO`, `LOG_WARNING`, etc. as integer constants, which collide with the `LOG(INFO)` macro from Chromium/glog-style logging in `base/logging.h`. If you see bizarre compile errors about `LOG` arguments being integers, use `apache_logging_includes.h` instead of directly including `http_log.h`.

## Cross-References

See root `CLAUDE.md` for:
- Apache system test status and known differences from other platforms
- Build commands and Docker development workflow
- ProcessContext lifecycle across all deployment modes
- IIS/Envoy/Nginx comparison tables

# pagespeed/iis/ -- IIS Native Module

Windows IIS module implementing PageSpeed optimization as a native IIS HTTP module DLL.

## Key Files

| File | Purpose |
|------|---------|
| `dll_main.cc` | DLL entry point (`DllMain`), `RegisterModule` export, ProcessContext init under loader lock |
| `iis_http_module.cpp` | `CHttpModule` subclass: request routing, admin handlers, HTML/resource/IPRO dispatch |
| `iis_module_base_fetch.cpp` | `IisModuleBaseFetch`: bridges PSOL async writes to IIS `WriteEntityChunkByReference` |
| `iis_rewrite_driver_factory.cpp` | Factory: creates server contexts, allocates fetchers, tunes worker pools per CPU count |
| `iis_process_context.cpp` | Singleton `IisProcessContext`: owns driver factory, server context, lifecycle management |
| `iis_configuration.cpp/.h` | Config file parser (`pagespeed.config`/`iiswebspeed.config`), RE2 match rules, timestamp-based config reload |
| `asyncwinhttp.cpp` | `WinHTTP` async HTTP client: callback-driven fetching via Windows WinHTTP API |
| `iis_misc.cpp` | Utilities: `s2ws`/`ws2s` encoding, `determine_options`, experiment cookie, response writing |
| `iis_module_factory.cpp` | `IHttpModuleFactory`: creates per-request `IisHttpModule` instances |
| `iis_module_request_context.h` | Per-request state container attached to IIS request via `SetModuleContext` |

## IIS Request Pipeline Integration

The module registers request notifications `RQ_SEND_RESPONSE | RQ_BEGIN_REQUEST | RQ_CUSTOM_NOTIFICATION` and a separate global notification in `RegisterModule` (`dll_main.cc`):

- **`RQ_BEGIN_REQUEST`** (priority `PRIORITY_ALIAS_FIRST`) -- Routes requests: resource fetch, IPRO lookup, admin, beacon, or HTML rewrite. Resource/admin requests are handled immediately; HTML requests set up ProxyFetch.
- **`RQ_SEND_RESPONSE`** (priority `PRIORITY_ALIAS_LAST`) -- Intercepts origin response. For HTML: captures body, feeds it to ProxyFetch. For IPRO: records response for in-place optimization.
- **`RQ_CUSTOM_NOTIFICATION`** -- Custom notification support for async completion signaling.
- **`GL_PRE_BEGIN_REQUEST`** (global, `PRIORITY_ALIAS_FIRST`) -- `MyGlobalModule` preserves `X-PRISTINE-URL` before URL rewrite modules modify the request URL.

## State Machine Flags

The request lifecycle is controlled by several interdependent flags:

- **`pending`** (`RQ_NOTIFICATION_PENDING`) -- Returned from `OnSendResponse` to defer IIS completion while PSOL rewrites. Module MUST call `IndicateCompletion()` exactly once.
- **`base_fetch.pending_`** -- Tracks whether IIS is waiting for PSOL output. When true, `HandleFlush` writes data directly; when false, data is buffered.
- **`end_request_seen`** -- Set when IIS signals request teardown. Prevents double-free of request context.
- **`http_context_`** -- Nulled immediately after `IndicateCompletion()`. Any post-completion access crashes.

## AsyncWinHttp

Uses Windows WinHTTP API (not libcurl) because the IIS module runs inside `w3wp.exe` where:
- Windows-native async I/O integrates with the IIS thread pool model
- libcurl/BoringSSL are not available on Windows (BCrypt is used for crypto instead)
- WinHTTP handles proxy settings, SSL, and auth via Windows credential store

The fetcher is fully async with callback-driven state machine (`OnHeadersAvailable`, `OnDataAvailable`, `OnRequestError`) and configurable timeouts (resolve/connect/send/receive/total).

## Build

```powershell
bazel build --config=vendored --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll
```

## Test

```powershell
# C++ unit tests
bazel test --config=vendored --config=windows --config=clang-cl //test/pagespeed/iis/...

# Python integration tests (requires IIS Express running)
pytest test/iis/ -v
# Environment: PAGESPEED_PORT=8080, IIS_EXPRESS=1, PAGESPEED_TEST_ROOT=, PAGESPEED_EXAMPLE_ROOT=
```

## Debug

- **WinDbg**: attach to `w3wp.exe` (the IIS worker process hosting the DLL)
- **ASan logs**: `C:\pagespeed_asan*` (ASan options embedded in `dll_main.cc` via `__asan_default_options`)
- **Event tracing**: `IisMessageHandler` routes to IIS tracing infrastructure

## Cross-References

See root `CLAUDE.md` section "IIS Platform Internals" for:
- String encoding rules (`CP_ACP` vs `CP_UTF8`)
- Request lifecycle and `RQ_NOTIFICATION_PENDING` semantics
- Frozen `RewriteOptions` constraints
- `__x_` header prefix convention
- `DllMain` loader lock restrictions

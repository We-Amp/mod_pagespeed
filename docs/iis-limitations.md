# IIS PageSpeed Known Limitations

**Status: Stable** (since 1.15.0+r18 — see the platform table in
`RELEASE_NOTES.md`; this doc previously said Experimental, which reflected the
1.1.0-beta.1 era)

## Architecture

The module creates a **per-site** `IisProcessContext` (keyed by IIS
application id in `IisModuleFactory::site_contexts_`,
`iis_module_factory.cpp`), each owning its own `IisRewriteDriverFactory` and
`IisServerContext`. Sites do NOT share a server context or configuration:
per-site options come from `pagespeed.config` match rules, and when only a
global `FileCachePath` is configured, each site's cache is placed in a
per-site subdirectory (`<FileCachePath>/<site_app_id>`,
`iis_process_context.cpp`).

Configuration is read from `pagespeed.config` (legacy fallback name:
`iiswebspeed.config`) — resolved against the site's application root and
`%ProgramData%\We-Amp\PageSpeed\` (legacy `%ProgramData%\We-Amp\IISWebSpeed\`
is still honored for upgrades from IISpeed / early 1.1 installs). Directives
use the standard `pagespeed <Option> <args>` form with RE2 match rules;
the file is re-read on timestamp change. As of r18, configuration parsing is
hardened: a malformed line cannot crash the module at startup, unknown
options are reported instead of silently ignored, and option scoping is
enforced on IIS as on the other ports.

### Startup diagnostics (1.1.0+r11 era)

- The cache directory is auto-created (with a conditional ACL grant for the
  worker identity) when it lives under a canonical prefix
  (`PageSpeed\cache\` / `IISWebSpeed\cache\`); `AutoCreateLogDir` (default
  on) does the same for `LogDir`.
- On init failure, local requests get a diagnostic page plus a
  machine-readable `X-Pagespeed-Init-Status` response header with one of:
  `cache-path-empty | cache-path-missing | cache-path-not-writable |
  cache-path-create-failed | log-dir-create-failed | post-config-failed |
  startup-failed` (`iis_http_module.cpp`).

## Functional Limitations

### HTML Flushing
- Incremental flushing of rewritten HTML is compiled out (`IISPEEDHASFLUSH`
  is not defined in `pagespeed/iis/BUILD`); rewritten HTML output is
  buffered rather than flushed in windows
- `iis_html_flushing_integration_test` is excluded from the build for the
  same reason (see `docs/test-catalog.md`)

### Forbidden Filters
- `convert_meta_tags` is forbidden (`iis_rewrite_driver_factory.cpp`): the
  module does not see the response body before response headers go out

### Shared Memory
- The module wires `InProcessSharedMem` into the factory
  (`iis_process_context.cpp`), NOT a cross-process shared-memory runtime.
  Shared-memory statistics, the shm metadata cache, and the message
  circular buffer are therefore **per worker process** — multiple `w3wp.exe`
  workers (web gardens, overlapping app-pool recycles) do not share them.
  The per-process default shm cache is tuned down to 5 MB for this reason
  (`iis_rewrite_driver_factory.cpp`).
- A true `WindowsSharedMem` implementation exists
  (`pagespeed/kernel/sharedmem/windows_shared_mem.cc`, unit-tested) but is
  not wired into the IIS module: the shared-memory cache metadata assumes
  8-byte mutex alignment, which Windows x64 violates (`CRITICAL_SECTION`
  requires 16-byte alignment), so wiring it in safely takes a per-platform
  layout pass that has not happened yet

### External Cache
- Redis only. Memcached support is compile-time excluded on non-Linux
  platforms (`pagespeed/system/system.bzl`: `memcached_cache.cc` is
  Linux-only, `-DPAGESPEED_ENABLE_MEMCACHED=0` elsewhere; libmemcached does
  not build on Windows)

### Resolved since the beta-era version of this doc

These were real limitations when this doc was written (1.1.0-beta.1,
2026-02) and are listed here so stale copies of the old claims don't
propagate:

- **IPRO caches** — IPRO now goes through the standard system cache stack
  like every other port: `SystemRewriteDriverFactory::RootInit/ChildInit`
  wire `SystemCaches` (Cyclone-backed file cache, per-process shm/LRU
  tiers, optional Redis), IPRO lookups use
  `RewriteDriver::FetchInPlaceResource`, and responses are recorded via
  `InPlaceResourceRecorder` (`iis_http_module.cpp`,
  `iis_process_context.cpp`). The old "async cache stubs return misses /
  re-optimized on each request" behavior is gone since the IISpeed code
  base was adopted for the IIS port.
- **Beacon data** — no longer discarded. `/mod_pagespeed_beacon`
  GET+POST requests are routed (`RequestRouting::kBeacon`) and handed to
  `ServerContext::HandleBeacon` with a POST size cap; the factory returns
  `UseBeaconResultsInFilters() == true`, so beacon-driven filters
  (critical CSS, critical images) are active on IIS. The r17/r18
  critical-CSS beacon work (viewport-aware beacon, truncation statistics)
  lives in shared code and applies to IIS. One wrinkle: a failed
  `HandleBeacon` still returns 204 (see the TODO in
  `iis_http_module.cpp`), so beacon rejection is not client-visible.
- **Zero-copy serve** — memory-mapped (Cyclone) cache hits are served
  zero-copy on IIS as of r18 (`CycloneZeroCopyServe`;
  `iis_zerocopy_serve.h`, `iis_module_base_fetch.cpp`).

## Configuration Caveats

### Module Registration
- If registered globally via `New-WebGlobalModule`, do NOT add the module
  again in site `web.config` -- causes 500.19 "duplicate collection entry"

### allowDoubleEscaping
- Must be set to `true` in `requestFiltering` configuration
- Combined resource URLs contain `+` characters (e.g.,
  `a.css+b.css.pagespeed.cc.HASH.css`) that trigger IIS
  RequestFilteringModule 404.11 errors

### web.config vs pagespeed.config
- `web.config` is only involved for IIS-level plumbing (module registration,
  `requestFiltering`); PageSpeed directives themselves live in
  `pagespeed.config` (see Architecture above). Older docs describing
  filter configuration "through web.config XML" are out of date.

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
| Shared Memory | InProcessSharedMem (per worker process) | PthreadSharedMem (cross-process) |
| External Cache | Redis only | Redis + Memcached (Linux-only) |
| Config Format | `pagespeed.config` directives (+ `web.config` for IIS plumbing) | httpd.conf directives |
| Sub-resource fetcher | WinHTTP (async) | libcurl (`CurlUrlAsyncFetcher`) |
| HTML flush windows | Compiled out | Supported |

## Tests

The IIS suite lives in `test/iis/` (Python integration, now ~30 test files)
and `test/pagespeed/iis/` (C++ unit tests); see `docs/test-catalog.md` for
the current catalog. The pass counts previously listed here (19/19 C++,
350/356 Python with a per-file table) were a 1.1.0-beta.1 snapshot of a much
smaller suite and have been removed rather than left to rot. Known standing
exclusion: `iis_html_flushing_integration_test` (flushing compiled out, see
above).

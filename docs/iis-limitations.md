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
  cache-path-create-failed | log-dir-create-failed | daemon-volume-split |
  post-config-failed | startup-failed` (`iis_http_module.cpp`).
- `daemon-volume-split`: the site set both `DaemonSocketPath` and
  `DaemonVolumePath`, and the module's startup check found that opening the
  daemon's cache volume at the size the daemon published created a second
  volume file instead of attaching to the daemon's. The site then engages no
  PageSpeed at all — nothing silently optimizes from the wrong cache — and one
  Windows event-log entry is written under the `PageSpeed` event source; when
  that source is not registered on the machine, Event Viewer still shows the
  entry, prefixed with a note that the description cannot be found. The
  verdict lives until the application pool recycles: a configuration edit that
  leaves both paths unchanged does not re-probe, and a daemon that is not
  answering when the first site with this configuration starts in a worker
  process leaves in-place optimization off for those sites until the pool
  recycles (the other ports need a web-server restart for the same). The
  check removes the volume file its own open created; the event-log entry's
  appended report says so, or names the file to remove by hand when it could
  not. Recovery: start or restart the optimizer daemon and let it create its
  volume, then recycle the application pool. If the site still refuses after
  that, the module and the daemon disagree about the volume: install a module
  and optimizer package pair that agree. If the event-log entry says the file
  could not be removed, remove it before the pool recycles: a worker process
  that finds it attaches to it without refusing. The check runs when EITHER option is set —
  one set alone leaves in-place optimization off, with one log line — and
  with either option set the classic in-place recorder is not used for the
  site, whatever the check finds; with both unset the module behaves exactly
  as before.

### Event Logging (`UseEventLog`)

- `pagespeed UseEventLog on` (in `pagespeed.config`) routes the module's
  WARNING and ERROR messages to the Windows Application event log under the
  `PageSpeed` source. INFO messages never reach the event log (on this port
  they are per-request volume); a FATAL is always written, whatever the
  directive says. With the directive off or unset, the first suppressed
  warning and the first suppressed error each write ONE entry saying
  event-log writing is off, how to turn it on, and that the messages can
  also be read at the admin message history path (`/pagespeed_message` by
  default) from the local machine.
- Messages raised before the directive's value is KNOWN in a worker process
  are not written, whatever the directive says; a FATAL is the exception.
  Where the directive sits at the top level of the configuration, the value
  is known as soon as the first configuration is read, so the module's
  start-up messages are written. Where it sits inside a host- or
  path-matched block, the value is known when that site's configuration is
  read, and the start-up messages before that are only in the admin message
  history. The setting is one per worker process, so a site that sets
  nothing follows whatever a site in the same process set.
- Volume: at most one entry per distinct message text per worker process,
  and a hard cap of 1000 entries per worker process; past the cap one final
  entry says so and nothing further is written (a FATAL included) until the
  application pool recycles.
- The `PageSpeed` event source is not registered by the installer yet, so
  entries appear with Event Viewer's "The description for Event ID … cannot
  be found" preface above the message text; the message itself is intact.
  Registering the source is a later installer change.

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

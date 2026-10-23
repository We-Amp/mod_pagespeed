# pagespeed/envoy/

Envoy HTTP filter that integrates the PageSpeed optimization engine as a
native Envoy stream filter. Rewrites HTML, CSS, JS, and images in-flight.

## Key Files

| File | Purpose |
|------|---------|
| `http_filter_config.cc` | Filter factory registration, proto config parsing, lazy `ProcessContext` init |
| `http_filter.cc` / `http_filter.h` | `HttpPageSpeedDecoderFilter` -- stream filter implementing `decodeHeaders`/`encodeData` |
| `http_filter.proto` | Protobuf config schema (`pagespeed.Decoder`) |
| `envoy_rewrite_driver_factory.cc` | Factory subclass; allocates CurlUrlAsyncFetcher, manages scheduler threads |
| `envoy_server_context.cc` | Per-server state, VHost option lookup and merged-options cache |
| `envoy_base_fetch.h` | `EnvoyBaseFetch` -- AsyncFetch adapter with atomic refcount (starts at 2) |
| `envoy_async_fetch.cc` | Bridges ProxyFetch completion back to Envoy's dispatcher thread |
| `envoy_process_context.cc` | Singleton wrapper around PSOL `ProcessContext` |
| `envoy_vhost_config.cc` | Per-virtual-host rewrite options, matched by host pattern |
| `envoy_dispatcher_adapter.cc` | Adapts Envoy's `Event::Dispatcher` to PSOL's `EventDispatcher` interface |
| `envoy_metrics_collector.cc` | Prometheus metrics (`pagespeed.html_rewrites_total`, etc.) |

## Filter Architecture

`HttpPageSpeedDecoderFilterConfig` (registered as `"pagespeed"`) implements
`NamedHttpFilterConfigFactory`. On the first `createFilterFactoryFromProto`
call it lazily initializes the global `EnvoyProcessContext` singleton, which
owns the `EnvoyRewriteDriverFactory` and `EnvoyServerContext`.

The returned `Http::FilterFactoryCb` lambda captures the proto config and
creates a new `HttpPageSpeedDecoderFilter` per stream, wired to the
singleton `ProxyFetchFactory`.

## Configuration

Production config lives at `pagespeed-envoy.yaml` in the repository root.
The filter config block uses `@type: type.googleapis.com/pagespeed.Decoder`
and supports Redis/Memcached cache backends, file cache, domain mapping,
virtual host overrides, circuit breaker, and admin authentication.

## Build Notes

The filter builds against Envoy 1.40, which `MODULE.bazel` declares as a
module (`envoy` and `envoy_api`, both an `archive_override` of the v1.40.1
commit; Envoy's registry follows the BCR in `.bazelrc`). Build with
`--config=envoy-filter` (clang + libstdc++). Every target here and in
`test/pagespeed/envoy` is tagged manual, so `//...` leaves the filter out
(building it compiles all of Envoy); name the targets:

```
bazel build --config=envoy-filter //pagespeed/envoy:envoy_pagespeed      # standalone binary
bazel build --config=envoy-filter //pagespeed/envoy:pagespeed_filter.so  # shared library
bazel test --config=envoy-filter //test/pagespeed/envoy:http_filter_test # unit tests (tagged manual, so named)
```

The extension list Envoy compiles in is `bazel/envoy/extensions_build_config.bzl`,
served to Envoy as its `envoy_build_config` repository through `override_repo`
in `MODULE.bazel`.

The filter links one copy of each library it shares with Envoy: abseil,
BoringSSL, protobuf, fmt, spdlog and googleurl are single modules in the graph
(googleurl is our newer snapshot, overridden for Envoy too). Our libevent
(`com_github_libevent_libevent`, vanilla 2.1.12) is separate from Envoy's
`libevent` module; the filter does not link ours (only the Apache dispatcher
does).

Envoy's build macros name some repositories with plain string labels that
resolve in the calling repository, so the root module declares them under
Envoy's names too (`protobuf`, `re2`, `googleapis`, `protoc-gen-validate`).

## Testing

```bash
./test/system/run_envoy_tests.sh           # full suite
./test/system/run_envoy_tests.sh -k sanity # quick check
```

See `docs/test-catalog.md` for current pass/skip counts.

## Threading Model

ProxyFetch and rewrite workers run on PSOL thread pools. Completion is
signaled back to Envoy's main thread via `dispatcher_.post()`. Scheduler
alarms (rewrite deadlines, fetch timeouts) are driven by the Envoy
dispatcher: `EnvoyRewriteDriverFactory::CreateScheduler()` returns an
`EventScheduler`, and `StartThreads()` attaches the dispatcher adapter so
the event loop pumps `RunAlarms()` — alarm callbacks run ON the dispatcher
thread and must stay cheap (a tracked limitation).
`EnvoyAsyncFetch` uses `shared_from_this()` to prevent use-after-free when
posting lambdas to the dispatcher thread. `EnvoyBaseFetch` uses a manual
`std::atomic<int> references_` refcount (initial value 2: one for Envoy,
one for PSOL). The `decoder_` pointer is also atomic to guard against races
between DetachDecoder and HandleDone.

## Why libcurl for Fetching

Resource fetching uses `CurlUrlAsyncFetcher` (libcurl) rather than Envoy's
ClusterManager. This avoids TLS/BoringSSL initialization conflicts that
previously blocked HTML rewriting and keeps the fetcher independent of
Envoy's cluster configuration. An optional `CircuitBreakerFetcher` wrapper
provides resilience around the curl fetcher.

## Cross-References

See root `CLAUDE.md` section "Envoy Filter" for:
- Filter architecture and data flow diagram
- Production configuration details (`pagespeed-envoy.yaml`)
- System test status and known differences from Apache
- libcurl fetcher rationale

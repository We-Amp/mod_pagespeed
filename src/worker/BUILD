# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# PSOL Factory Worker

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# Header-only libuv handle safety helpers (SafeClose, SafeTimerClose)
cc_library(
    name = "uv_helpers",
    hdrs = ["uv_helpers.h"],
    visibility = ["//visibility:public"],
    deps = ["@libuv"],
)

# POSIX compatibility layer for Windows (MSVC)
# Thin wrapper — actual implementation lives in //lib/base:posix_compat.
# Syscall-filter positive control: a switch that deliberately makes a call
# the enforcing syscall profile denies, so "the filter is installed and it
# kills" is observable and not inferred.  TWO libraries over ONE source --
# the shipped default (switch compiled out; the flag string is not even in
# the binary) and the enabled variant the unit test links.  local_defines,
# not defines, so the macro cannot leak to a dependent.  Building both every
# run is what stops the disabled default from drifting into code that no
# longer compiles.
cc_library(
    name = "pipe_dacl",
    srcs = ["pipe_dacl.cc"],
    hdrs = ["pipe_dacl.h"],
    linkopts = select({
        "@platforms//os:windows": ["advapi32.lib"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "windows_service",
    srcs = ["windows_service.cc"],
    hdrs = ["windows_service.h"],
    linkopts = select({
        "@platforms//os:windows": ["advapi32.lib"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "syscall_selftest",
    srcs = ["syscall_selftest.cc"],
    hdrs = ["syscall_selftest.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "syscall_selftest_enabled",
    testonly = True,
    srcs = ["syscall_selftest.cc"],
    hdrs = ["syscall_selftest.h"],
    local_defines = ["PAGESPEED_SELFTEST"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "posix_compat",
    hdrs = ["posix_compat.h"],
    visibility = ["//visibility:private"],
    deps = ["//lib/base:posix_compat"],
)

# HTML Scanner - read-only element collector for Critical CSS
cc_library(
    name = "html_scanner",
    srcs = ["html_scanner.cc"],
    hdrs = ["html_scanner.h"],
    # Vendored lib/html headers name unused parameters (canonical idiom);
    # keep the warnings non-fatal in consumer TUs.
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:string_util",
        "//lib/base:url_util",
        "//lib/classify:url_normalizer",
        "//lib/css:css_import_flattener",
        "//lib/html:compat",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_node",
        "//lib/html:html_parse",
        "@com_google_absl//absl/strings",
    ],
)

# Diagnostic-only async-CSS force switch (CLI-only; see the header)
cc_library(
    name = "unsafe_force_async_css",
    hdrs = ["unsafe_force_async_css.h"],
    visibility = ["//visibility:public"],
)

# HTML CSS Injector - injects critical CSS into HTML
cc_library(
    name = "html_css_injector",
    srcs = ["html_css_injector.cc"],
    hdrs = ["html_css_injector.h"],
    visibility = ["//visibility:public"],
    deps = ["//lib/base:string_util"],
)

# <noscript> removal for the browser-analysis renders
cc_library(
    name = "noscript_strip",
    srcs = ["noscript_strip.cc"],
    hdrs = ["noscript_strip.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":html_css_injector",
        ":html_scanner",
    ],
)

# Cascade-layer order of a page, and where a layered critical block may go
cc_library(
    name = "cascade_layer_order",
    srcs = ["cascade_layer_order.cc"],
    hdrs = ["cascade_layer_order.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":html_css_injector",
        "//lib/base:string_util",
    ],
)

# The scanner's stylesheet sources as cascade-layer inputs
cc_library(
    name = "layer_order_sources",
    srcs = ["layer_order_sources.cc"],
    hdrs = ["layer_order_sources.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cascade_layer_order",
        ":html_scanner",
        "@com_google_absl//absl/strings",
    ],
)

# CSS Cache Inliner - resolves CSS URLs from cache for inline injection
cc_library(
    name = "css_cache_inliner",
    srcs = ["css_cache_inliner.cc"],
    hdrs = ["css_cache_inliner.h"],
    # Vendored lib/html headers name unused parameters (canonical idiom);
    # keep the warnings non-fatal in consumer TUs.
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":html_scanner",
        "//lib/base:string_util",
        "//lib/base:string_writer",
        "//lib/base:url_util",
        "//lib/css:css_import_flattener",
        "//lib/html:compat",
        "//lib/html:empty_html_filter",
        "//lib/html:html_name",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
    ],
)

# Analysis Resource Map - same-origin cached script bytes for the offline
# analysis browser (Fetch interception), plus the <base href> injector
cc_library(
    name = "analysis_resource_map",
    srcs = ["analysis_resource_map.cc"],
    hdrs = ["analysis_resource_map.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":html_css_injector",
        ":html_scanner",
        "//lib/base:url_util",
        "//lib/classify:url_normalizer",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings",
    ],
)

# Critical CSS Extractor - extracts above-the-fold CSS
cc_library(
    name = "critical_css_extractor",
    srcs = ["critical_css_extractor.cc"],
    hdrs = ["critical_css_extractor.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cascade_layer_order",
        ":html_css_injector",
        ":html_scanner",
        "//lib/base:string_util",
        "//lib/classify:capability_mask",
        "//src/browser:device_emulation",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings",
    ],
)

# Font URL Extractor - extract font URLs from CSS @font-face blocks
cc_library(
    name = "font_url_extractor",
    srcs = ["font_url_extractor.cc"],
    hdrs = ["font_url_extractor.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:string_util",
        "@com_google_absl//absl/strings",
    ],
)

# Image Transcoder - format conversion based on capability mask
cc_library(
    name = "image_transcoder",
    srcs = ["image_transcoder.cc"],
    hdrs = ["image_transcoder.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:message_handler",
        "//lib/classify:capability_mask",
        "//lib/image:content_analyzer",
        "//lib/image:exif_orientation",
        "//lib/image:gif",
        "//lib/image:image_converter",
        "//lib/image:image_resizer",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:png",
        "//lib/image:quality_features",
        "//lib/image:quality_predictor",
        "//lib/image:quality_verifier",
        "//lib/image:read_image",
        "//lib/image:webp",
        "@libavif",
    ],
)

# Async-CSS loader — same-origin path + JS, shared by the worker transform,
# the nginx module, and the C ABI getters (single source of truth).
cc_library(
    name = "async_css_loader",
    hdrs = ["async_css_loader.h"],
    visibility = ["//visibility:public"],
)

# Subresource gate - whether a cached sub-resource is served to an anonymous
# client before another URL's output is built from it
cc_library(
    name = "subresource_gate",
    srcs = ["subresource_gate.cc"],
    hdrs = ["subresource_gate.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/classify:alternate_metadata",
        "//lib/classify:hostname",
        "//lib/net:fetch_policy",
        "//lib/net:upstream_pin",
        "//src/browser:agent_fetcher",
    ],
)

# HTML Transform Filter - unified HTML modifications (lazy load, dimensions,
# critical CSS)
cc_library(
    name = "html_transform_filter",
    srcs = ["html_transform_filter.cc"],
    hdrs = ["html_transform_filter.h"],
    # Vendored lib/html headers name unused parameters (canonical idiom);
    # keep the warnings non-fatal in consumer TUs.
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":async_css_loader",
        ":cascade_layer_order",
        ":html_css_injector",
        ":html_scanner",
        ":subresource_gate",
        "//lib/base:string_util",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/html:csp_inline_policy",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_node",
        "//lib/html:html_parse",
        "//lib/image:image_dimensions",
    ],
)

# Text Compressor - gzip and brotli compression for pre-compressed variants
cc_library(
    name = "text_compressor",
    srcs = ["text_compressor.cc"],
    hdrs = ["text_compressor.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@brotli//:brotlidec",
        "@brotli//:brotlienc",
        "@zlib",
    ],
)

# Browser Analysis Manager - Chrome lifecycle, analysis queue, profile storage
cc_library(
    name = "browser_analysis_manager",
    srcs = ["browser_analysis_manager.cc"],
    hdrs = [
        "browser_analysis_manager.h",
        "browser_analysis_manager_internal.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":analysis_resource_map",
        ":cascade_layer_order",
        ":critical_css_extractor",
        ":css_cache_inliner",
        ":html_scanner",
        ":noscript_strip",
        ":subresource_gate",
        ":uv_helpers",
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//lib/net:fetch_policy",
        "//lib/net:upstream_pin",
        "//src/browser:agent_fetcher",
        "//src/browser:analysis_queue",
        "//src/browser:browser_css_extractor",
        "//src/browser:browser_sandbox",
        "//src/browser:cdp_types",
        "//src/browser:chrome_process",
        "//src/browser:critical_css_validator",
        "//src/browser:device_emulation",
        "//src/browser:optimization_profile",
        "//src/browser:page_analysis",
        "//src/browser:script_coverage_analyzer",
        "//src/browser:template_detector",
        "//src/browser:visual_regression_gate",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@libuv",
    ],
)

# HTTP Management API Server - libuv TCP + llhttp parser
cc_library(
    name = "http_server",
    srcs = ["http_server.cc"],
    hdrs = ["http_server.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":posix_compat",
        ":uv_helpers",
        "//lib/base:message_handler",
        "//lib/base:string_util",
        "@com_google_absl//absl/strings",
        "@libuv",
        "@llhttp",
    ],
)

# Safe JSON serialization: the one choke point every .dump( call under
# src/worker/ must go through (test/src/worker:json_output_test enforces
# this with a source-tree gate), so every JSON response and stream message
# is always well-formed.
cc_library(
    name = "json_dump",
    hdrs = ["json_dump.h"],
    visibility = ["//visibility:public"],
    deps = ["@nlohmann_json//:json"],
)

# Package sources, exposed so the json_output_test regression gate can scan
# every current (and future) file under src/worker/ for a bare .dump( call
# that bypasses :json_dump.
filegroup(
    name = "all_sources",
    srcs = glob([
        "**/*.cc",
        "**/*.h",
    ]),
    visibility = ["//test/src/worker:__pkg__"],
)

# Known-sites file (site ids the worker has stored work for)
cc_library(
    name = "known_sites",
    srcs = ["known_sites.cc"],
    hdrs = ["known_sites.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:atomic_file_writer",
        "//src/proto:worker_ipc",
    ],
)

# In-memory URL registry for cache URL enumeration
cc_library(
    name = "url_registry",
    srcs = ["url_registry.cc"],
    hdrs = ["url_registry.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":known_sites",
        "//lib/classify:hostname",
    ],
)

# Cache inspection & mutation API handlers (/v1/cache/*)
cc_library(
    name = "cache_handlers",
    srcs = ["cache_handlers.cc"],
    hdrs = ["cache_handlers.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":http_server",
        ":json_dump",
        ":url_registry",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//lib/classify:pagespeed_selector",
        "//lib/classify:url_normalizer",
        "//src/proto:worker_ipc",
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)

# WebSocket protocol layer (RFC 6455 framing, handshake)
cc_library(
    name = "websocket",
    srcs = ["websocket.cc"],
    hdrs = ["websocket.h"],
    visibility = ["//visibility:public"],
)

# WebSocket endpoint handlers (/v1/ws/stats, /v1/ws/events)
cc_library(
    name = "ws_handlers",
    srcs = ["ws_handlers.cc"],
    hdrs = ["ws_handlers.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":json_dump",
        ":uv_helpers",
        ":websocket",
        "//lib/base:message_handler",
        "//lib/base:string_util",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

# Optimization policy engine (browser analysis -> optimization decisions)
cc_library(
    name = "optimization_policy",
    srcs = ["optimization_policy.cc"],
    hdrs = ["optimization_policy.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":json_dump",
        "//src/browser:optimization_profile",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@nlohmann_json//:json",
    ],
)

# Dedicated thread that saves configuration files off the event loop
cc_library(
    name = "config_persister",
    srcs = ["config_persister.cc"],
    hdrs = ["config_persister.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:message_handler",
        "@libuv",
    ],
)

# Config file persistence (hot-reloadable settings JSON)
cc_library(
    name = "config_file",
    srcs = ["config_file.cc"],
    hdrs = ["config_file.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":html_mode",
        ":json_dump",
        ":worker_types",
        "//lib/base:atomic_file_writer",
        "@nlohmann_json//:json",
    ],
)

# The worker's HTML mode (off, markdown-static, markdown-rendered, full) and
# its mapping from the legacy disable_html boolean.  Header-only, no deps: the
# nginx module's inline shared-config reader uses it too.
cc_library(
    name = "html_mode",
    hdrs = ["html_mode.h"],
    visibility = ["//visibility:public"],
)

# Shared config file (worker-written, nginx-read key=value config)
cc_library(
    name = "shared_config",
    srcs = ["shared_config.cc"],
    hdrs = ["shared_config.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":html_mode",
        "//lib/base:atomic_file_writer",
        "//lib/base:message_handler",
        "//lib/base:posix_compat",
    ],
)

# Cache directory startup validation: loud refusal to
# start when the directory is absent, unwritable, or foreign-owned.
cc_library(
    name = "cache_dir",
    srcs = ["cache_dir.cc"],
    hdrs = ["cache_dir.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:message_handler",
    ],
)

# Default thread count from the container CPU budget (affinity, cgroup quota).
cc_library(
    name = "cpu_budget",
    srcs = ["cpu_budget.cc"],
    hdrs = ["cpu_budget.h"],
    visibility = ["//visibility:public"],
)

# Host aliases file (worker-written, nginx-read hostname mapping)
cc_library(
    name = "host_aliases",
    srcs = ["host_aliases.cc"],
    hdrs = ["host_aliases.h"],
    visibility = ["//visibility:public"],
    deps = ["//lib/base:atomic_file_writer"],
)

# Web Bot Auth key-directory warmer: periodic SSRF-guarded
# JWKS fetch in the worker, published to nginx via an atomically-written
# keys file next to pagespeed-shared.conf.
cc_library(
    name = "webbotauth_warmer",
    srcs = ["webbotauth_warmer.cc"],
    hdrs = ["webbotauth_warmer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:atomic_file_writer",
        "//lib/net:fetch_policy",
        "//lib/net:ssrf_guard",
        "//src/browser:agent_fetcher",
        "//src/crypto/webbotauth:webbotauth_core",
        "@com_google_absl//absl/strings",
    ],
)

# Shared mmap serve-time bandwidth stats (nginx writes, worker reads)
cc_library(
    name = "serve_stats",
    srcs = ["serve_stats.cc"],
    hdrs = ["serve_stats.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":posix_compat",
        "//lib/base:message_handler",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
    ],
)

# Static file serving for web console SPA
cc_library(
    name = "static_file_handler",
    srcs = ["static_file_handler.cc"],
    hdrs = ["static_file_handler.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":http_server",
        "//lib/base:message_handler",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
    ],
)

# Worker type definitions (WorkerStats, WorkerConfig, AtomicSharedPtr, Worker).
# Header-only target that breaks the worker_lib ↔ api_handlers dependency
# cycle: worker_lib depends on api_handlers (to register HTTP endpoints),
# and api_handlers needs Worker* to access stats/config. This target exposes
# the Worker type without pulling in worker_lib's implementation. If this
# target ever gains a .cc file, the cycle will resurface.
cc_library(
    name = "worker_types",
    hdrs = ["worker.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":browser_analysis_manager",
        ":config_persister",
        ":html_mode",
        ":image_transcoder",
        ":serve_stats",
        ":webbotauth_warmer",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:url_normalizer",
        "//src/proto:worker_ipc",
        "@libuv",
    ],
)

# Read-only "is this process under a syscall filter?" answer,
# read from /proc/self/status and never from configuration.  Leaf target with
# no deps so it can be unit-tested on its own.
cc_library(
    name = "syscall_filter",
    srcs = ["syscall_filter.cc"],
    hdrs = ["syscall_filter.h"],
    visibility = ["//visibility:public"],
)

# Operational API handlers (/v1/health, /v1/stats, /v1/metrics, /v1/config)
cc_library(
    name = "api_handlers",
    srcs = ["api_handlers.cc"],
    hdrs = ["api_handlers.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":config_file",
        ":config_persister",
        ":http_server",
        ":json_dump",
        ":shared_config",
        ":worker_types",
        "//lib/base:message_handler",
        "//lib/classify:url_normalizer",
        "//src/browser:agent_fetcher",
        "//src/product_version:build_commit",
        "//src/product_version:version",
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)

# Capture API handlers (/v1/capture/waterfall, /v1/capture/screenshot)
cc_library(
    name = "capture_handlers",
    srcs = ["capture_handlers.cc"],
    hdrs = [
        "capture_handlers.h",
        "capture_handlers_internal.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":browser_analysis_manager",
        ":http_server",
        ":json_dump",
        "//lib/base:string_util",
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/browser:device_emulation",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

# LlmsTxtBuilder: synthesizes a customer's /llms.txt site-index.
# Pure of process effects — the own-origin fetch + agent-variant read are injected,
# so it is hermetically unit-testable.
cc_library(
    name = "llms_txt_builder",
    srcs = ["llms_txt_builder.cc"],
    hdrs = ["llms_txt_builder.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/base:string_util",
        "//lib/base:url_util",
        "//lib/classify:hostname",
        "//lib/html:html_metadata_extractor",
        "//lib/html:llms_txt_formatter",
        "//lib/html:robots_ai_directives",
        "//lib/html:sitemap_parser",
        "@cyclone",
    ],
)

cc_library(
    name = "worker_lib",
    srcs = ["worker.cc"],
    hdrs = ["worker.h"],
    # Vendored lib/html headers name unused parameters (canonical idiom);
    # keep the warnings non-fatal in consumer TUs.
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":api_handlers",
        ":browser_analysis_manager",
        ":cache_dir",
        ":cache_handlers",
        ":capture_handlers",
        ":cascade_layer_order",
        ":config_persister",
        ":critical_css_extractor",
        ":font_url_extractor",
        ":host_aliases",
        ":html_css_injector",
        ":html_mode",
        ":html_scanner",
        ":html_transform_filter",
        ":http_server",
        ":image_transcoder",
        ":known_sites",
        ":layer_order_sources",
        ":llms_txt_builder",
        ":optimization_policy",
        ":pipe_dacl",
        ":posix_compat",
        ":shared_config",
        ":static_file_handler",
        ":subresource_gate",
        ":syscall_filter",
        ":text_compressor",
        ":unsafe_force_async_css",
        ":url_registry",
        ":uv_helpers",
        ":webbotauth_warmer",
        ":ws_handlers",
        "//lib/base:message_handler",
        "//lib/base:string_util",
        "//lib/base:string_writer",
        "//lib/base:tee_message_handler",
        "//lib/base:url_util",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:option_context",
        "//lib/classify:url_normalizer",
        "//lib/css:css_import_flattener",
        "//lib/css:css_minify",
        "//lib/html:compat",
        "//lib/html:html_keywords",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//lib/image:content_analyzer",
        "//lib/image:image_util",
        "//lib/image:quality_verifier",
        "//lib/image:svg_rasterizer",
        "//lib/image:svg_vectorizer",
        "//lib/js:js_minify",
        "//lib/js:js_tokenizer",
        "//lib/net:fetch_policy",
        "//lib/net:upstream_pin",
        "//src/browser:agent_fetcher",
        "//src/browser:browser_css_extractor",
        "//src/browser:optimization_profile",
        "//src/browser:template_detector",
        "//src/proto:worker_ipc",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@cyclone",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

cc_binary(
    name = "factory_worker",
    srcs = ["main.cc"],
    linkopts = select({
        "@platforms//os:windows": [
            "ntdll.lib",
            "advapi32.lib",
        ],
        # Linux: static-link libc++/libc++abi/libunwind for the same reasons
        # as //lib/pagespeed:libpagespeed.so (see comment there). `-nostdlib++`
        # suppresses the clang driver's auto-injected dynamic `-lc++`; the
        # explicit `-Wl,-Bstatic ... -Wl,-Bdynamic` block pulls in the static
        # archives instead. `-Wl,-rpath,$$ORIGIN` is defence-in-depth.
        "@platforms//os:linux": [
            "-nostdlib++",
            "-Wl,-Bstatic",
            "-lc++",
            "-lc++abi",
            "-lunwind",
            "-Wl,-Bdynamic",
            "-Wl,-rpath,$$ORIGIN",
        ],
        "//conditions:default": [],
    }),
    # The test that runs this binary as a child process.
    visibility = ["//test/src/worker:__pkg__"],
    deps = [
        ":cache_dir",
        ":config_file",
        ":cpu_budget",
        ":html_mode",
        ":http_server",
        ":shared_config",
        ":syscall_selftest",
        ":unsafe_force_async_css",
        ":windows_service",
        ":worker_lib",
        "//lib/base:json_message_handler",
        "//lib/base:message_handler",
        "//src/browser:agent_fetcher",
        "//src/browser:browser_sandbox",
        "//src/product_version:build_commit",
        "//src/product_version:version",
        "@com_google_absl//absl/strings",
    ],
)

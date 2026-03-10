load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# =============================================================================
# Admin Console SPA embedding
# =============================================================================
# The Svelte 5 SPA is built outside Bazel (pagespeed/system/console/build.sh)
# and the output admin_console.html is checked in for hermetic builds.
# This genrule converts it to a C++ string constant via data2c.
genrule(
    name = "html_admin_console",
    srcs = ["console/admin_console.html"],
    outs = ["html_admin_console.cc"],
    cmd = "$(location //net/instaweb/js:data2c) --data_file=$< --c_file=$@ --varname=HTML_admin_console",
    tools = ["//net/instaweb/js:data2c"],
)

licenses(["notice"])  # Apache 2

# =============================================================================
# Common source file lists
# =============================================================================
# These lists are shared between system_envoy, system_windows, and system
# targets to reduce duplication and ensure consistency.

# Core source files used by all system library variants (envoy, windows, full).
# Excludes controller_manager.cc (POSIX fork/signals) and memcached_cache.cc
# (libmemcached is POSIX-only).
SYSTEM_CORE_SRCS = [
    "add_headers_fetcher.cc",
    "admin_license_handler.cc",
    "admin_site.cc",
    "external_server_spec.cc",
    "in_place_resource_recorder.cc",
    "loopback_route_fetcher.cc",
    "redis_cache.cc",
    "system_cache_path.cc",
    "system_caches.cc",
    "system_message_handler.cc",
    "system_request_context.cc",
    "system_rewrite_driver_factory.cc",
    "system_rewrite_options.cc",
    "system_server_context.cc",
    "system_thread_system.cc",
]

# Generated admin assets used by all system library variants.
# The old Closure Compiler CSS/JS assets have been replaced by the
# Svelte 5 SPA (admin_console.html), embedded via data2c.
SYSTEM_GENERATED_SRCS = [
    ":html_admin_console",
]

# Core header files used by all system library variants.
# These headers are public API for system library users.
SYSTEM_CORE_HDRS = [
    "add_headers_fetcher.h",
    "admin_license_handler.h",
    "admin_site.h",
    "controller_manager.h",
    "controller_process.h",
    "external_server_spec.h",
    "in_place_resource_recorder.h",
    "loopback_route_fetcher.h",
    "redis_cache.h",
    "system_cache_path.h",
    "system_caches.h",
    "system_message_handler.h",
    "system_request_context.h",
    "system_rewrite_driver_factory.h",
    "system_rewrite_options.h",
    "system_server_context.h",
    "system_thread_system.h",
]

# Core dependencies used by all system library variants.
SYSTEM_CORE_DEPS = [
    ":external_server_spec",
    ":redis_cache",
    ":system_controller_process_h",
    "//net/instaweb/http",
    "//net/instaweb/rewriter",
    "//pagespeed/kernel/cache:cyclone_cache",
    "//pagespeed/kernel/license_v2:license_file",
    "//pagespeed/kernel/license_v2:license_token",
    "//pagespeed/kernel/license_v2:license_verifier",
    "//pagespeed/kernel/sharedmem",
    "//third_party/redis-crc",
    "@hiredis",
]

# =============================================================================
# Admin license handler
# =============================================================================
# Handles /v1/license/* endpoints in the admin console.
# Separate library because it depends on license_v2 (Ed25519).
cc_library(
    name = "admin_license_handler",
    srcs = ["admin_license_handler.cc"],
    hdrs = ["admin_license_handler.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/license_v2:license_file",
        "//pagespeed/kernel/license_v2:license_token",
        "//pagespeed/kernel/license_v2:license_verifier",
    ],
)

# =============================================================================
# Fine-grained cache backend targets for faster incremental builds
# =============================================================================

# Shared utility for external server specifications (Redis/Memcached).
# No external dependencies - can be used independently.
cc_library(
    name = "external_server_spec",
    srcs = ["external_server_spec.cc"],
    hdrs = ["external_server_spec.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Circuit breaker for resource fetching reliability.
# Implements the circuit breaker pattern with CLOSED, OPEN, and HALF_OPEN states.
cc_library(
    name = "circuit_breaker",
    srcs = ["circuit_breaker.cc"],
    hdrs = ["circuit_breaker.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Circuit breaker wrapper for URL fetching.
# Wraps a UrlAsyncFetcher to apply circuit breaker logic.
cc_library(
    name = "circuit_breaker_fetcher",
    srcs = ["circuit_breaker_fetcher.cc"],
    hdrs = ["circuit_breaker_fetcher.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":circuit_breaker",
        "//net/instaweb/http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
    ],
)

# Redis cache backend. Depends on hiredis.
cc_library(
    name = "redis_cache",
    srcs = ["redis_cache.cc"],
    hdrs = ["redis_cache.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/thread",
        "//third_party/redis-crc",
        "@hiredis",
    ],
)

# Memcached cache backend. Depends on libmemcached.
cc_library(
    name = "memcached_cache",
    srcs = ["memcached_cache.cc"],
    hdrs = ["memcached_cache.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//bazel:libmemcached",
    ],
)

# =============================================================================
# Cache backend shared libraries (for dynamic loading)
# =============================================================================
# These can be loaded at runtime using dlopen/dlsym for configurations that
# need specific cache backends without linking them into the main binary.

cc_binary(
    name = "redis_cache.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":redis_cache"],
)

cc_binary(
    name = "memcached_cache.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":memcached_cache"],
)

# =============================================================================
# Envoy-optimized system library
# =============================================================================

# Minimal system library for Envoy deployments.
# Excludes components not needed by Envoy:
#   - controller_manager.cc (Envoy handles process management)
#   - memcached_cache.cc (Redis is the preferred external cache)
# This reduces binary size and external dependencies.
cc_library(
    name = "system_envoy",
    srcs = SYSTEM_CORE_SRCS + SYSTEM_GENERATED_SRCS,
    hdrs = SYSTEM_CORE_HDRS,
    copts = [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
        "-DPAGESPEED_NO_FORK",  # Use stub ControllerManager
    ],
    visibility = ["//visibility:public"],
    deps = SYSTEM_CORE_DEPS,
)

# =============================================================================
# Windows-compatible system library
# =============================================================================

# Minimal system library for Windows cross-compilation.
# Excludes POSIX-only components:
#   - controller_manager.cc (uses fork/signals)
#   - memcached_cache.cc (libmemcached is POSIX-only)
# Use this target for Windows IIS integration.
cc_library(
    name = "system_windows",
    srcs = SYSTEM_CORE_SRCS + SYSTEM_GENERATED_SRCS,
    hdrs = SYSTEM_CORE_HDRS,
    copts = [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
        "-DPAGESPEED_NO_FORK",  # Redundant with _WIN32, but explicit for clarity
    ],
    visibility = ["//visibility:public"],
    deps = SYSTEM_CORE_DEPS,
)

# =============================================================================
# Main system library (backward-compatible)
# =============================================================================

# Core system library - APR-free.
# Used by both Apache and Envoy deployments.
# Includes controller_manager.cc and memcached_cache.cc which are excluded
# from system_envoy and system_windows.
cc_library(
    name = "system",
    srcs = SYSTEM_CORE_SRCS + SYSTEM_GENERATED_SRCS + [
        "controller_manager.cc",
        "memcached_cache.cc",
    ],
    hdrs = SYSTEM_CORE_HDRS + [
        "memcached_cache.h",
    ],
    visibility = ["//visibility:public"],
    deps = SYSTEM_CORE_DEPS + [
        ":memcached_cache",
        "//bazel:libmemcached",
    ],
)


cc_library(
    name = "curl_fetcher",
    srcs = [
        "curl_fetch.cc",
        "curl_url_async_fetcher.cc",
    ],
    hdrs = [
        "curl_fetch.h",
        "curl_url_async_fetcher.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//net/instaweb:net_instaweb_lib",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//bazel:curl",
    ],
)

cc_library(
    name = "system_controller_process_h",
    srcs = [
    ],
    hdrs = [
        "controller_process.h",
    ],
    visibility = ["//visibility:public"],
)

# System-level test scripts for Apache/Nginx system tests
filegroup(
    name = "system_test_files",
    srcs = [
        "system_test.sh",
    ] + glob(["system_tests/*.sh"]),
    visibility = ["//visibility:public"],
)

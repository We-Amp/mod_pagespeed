load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load(":system.bzl", "pagespeed_system_library", "SYSTEM_CORE_SRCS", "SYSTEM_GENERATED_SRCS", "SYSTEM_CORE_HDRS", "SYSTEM_CORE_DEPS")

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

# Common source file lists (SYSTEM_CORE_SRCS, SYSTEM_GENERATED_SRCS,
# SYSTEM_CORE_HDRS, SYSTEM_CORE_DEPS) are loaded from system.bzl above.
# Edit them there — that is the single source of truth.

# =============================================================================
# JSON field extraction utilities (used by admin_license_handler).
# JsonEscape() now lives in string_util.h.
# =============================================================================
cc_library(
    name = "json_utils",
    srcs = ["json_utils.cc"],
    hdrs = ["json_utils.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# the design record over-cap host matching (PSL-free, header-only).
# Reimplemented byte-faithful from ModPageSpeed 2.0 (NOT vendored — these live
# in the engine, not the crypto sync boundary). Standalone so both the system
# library and over_cap_match_test depend on a single owner.
# =============================================================================
cc_library(
    name = "over_cap_match",
    hdrs = ["over_cap_match.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# Admin license handler
# =============================================================================
# Handles /v1/license/* endpoints in the admin console.
# Separate library because it depends on license_v2 (Ed25519).
cc_library(
    name = "admin_license_handler",
    srcs = ["admin_license_handler.cc"],
    hdrs = ["admin_license_handler.h"],
    local_defines = ["PAGESPEED_SERVER=\\\"unknown\\\""],
    visibility = ["//visibility:public"],
    deps = [
        ":json_utils",
        "//net/instaweb/http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/license_v2:license_file",
        "//pagespeed/kernel/license_v2:license_token",
        "//pagespeed/kernel/license_v2:license_verifier",
        "//pagespeed/kernel/license_v2:tracking_metadata",
    ],
)

# Test variant with zero-seed Ed25519 key for integration testing.
# Tokens signed with CreateKeypair(seed=zeros) will verify successfully.
cc_library(
    name = "admin_license_handler_testkey",
    testonly = True,
    srcs = ["admin_license_handler.cc"],
    hdrs = ["admin_license_handler.h"],
    local_defines = ["PAGESPEED_SERVER=\\\"test\\\""],
    visibility = ["//visibility:public"],
    deps = [
        ":json_utils",
        "//net/instaweb/http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/license_v2:license_file",
        "//pagespeed/kernel/license_v2:license_token",
        "//pagespeed/kernel/license_v2:license_verifier_testkey",
        "//pagespeed/kernel/license_v2:tracking_metadata",
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

# Memcached cache backend. Depends on libmemcached (Linux-only; libmemcached
# cannot build on macOS due to Envoy's -UDEBUG copt).
cc_library(
    name = "memcached_cache",
    srcs = ["memcached_cache.cc"],
    hdrs = ["memcached_cache.h"],
    target_compatible_with = ["@platforms//os:linux"],
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
    target_compatible_with = ["@platforms//os:linux"],
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
        "-DPAGESPEED_SERVER=\\\"envoy\\\"",
        "-Wno-trigraphs",  # Generated html_admin_console.cc contains ??X sequences
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
        "-DPAGESPEED_SERVER=\\\"iis\\\"",
        "-Wno-trigraphs",  # Generated html_admin_console.cc contains ??X sequences
    ],
    visibility = ["//visibility:public"],
    deps = SYSTEM_CORE_DEPS + [
        "//pagespeed/kernel/thread:win_thread",
    ],
)

# =============================================================================
# Main system library (nginx) and Apache variant
# =============================================================================

# Core system library - APR-free.
# Includes controller_manager.cc and memcached_cache.cc which are excluded
# from system_envoy and system_windows.
# Generated via pagespeed_system_library() macro in system.bzl.
pagespeed_system_library(name = "system", server_type = "nginx")

# Apache-specific variant. Reports server_type="apache" to the license service.
pagespeed_system_library(name = "system_apache", server_type = "apache")


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

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
# the design record optimization thread-count policy.
# Standalone so the effective-CPU-budget detection and the budget arithmetic
# can be unit-tested without a server, a container, or a particular host.
# =============================================================================
cc_library(
    name = "optimization_thread_policy",
    srcs = ["optimization_thread_policy.cc"],
    hdrs = ["optimization_thread_policy.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# The daemon health verdict, on its own, with no dependencies.
#
# Split out so a serving seam can read the verdict without taking on the
# run-time library binding that produces it -- or the link flag that binding
# needs. Every port has a serving seam; only the ports that actually run a
# startup check depend on :daemon_adapter.
# =============================================================================
cc_library(
    name = "daemon_health",
    hdrs = ["daemon_health.h"],
    visibility = ["//visibility:public"],
)

# =============================================================================
# The DaemonReader seam: the read-only interface between the
# admin console's /v1/daemon/* endpoints and the optimizer daemon's
# management API, on its own with no curl dependency.  Header-only so every
# port (including IIS, which has no daemon transport) can name the type; the
# UDS implementation lives in :curl_fetcher.
# =============================================================================
cc_library(
    name = "daemon_reader",
    hdrs = ["daemon_reader.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# The in-place recording lifecycle, as an interface.
#
# There are two substrates that record an origin response -- this module's own
# cache, and the optimizer daemon's shared one -- and they share nothing but
# the order a server drives them in. That order is the server's, not either
# implementation's, which is what makes it worth naming as a type.
#
# Header-only and on its own so the daemon-side implementation can depend on
# the lifecycle without depending on the system library, which depends on it.
# =============================================================================
cc_library(
    name = "ipro_recorder",
    srcs = ["ipro_recorder.cc"],
    hdrs = ["ipro_recorder.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
    ],
)

# =============================================================================
# Optimizer-daemon adapter: configuration, the volume-sizing mirror, and the
# health verdict the serving seams read.
#
# LICENSING BOUNDARY.  The optimizer daemon and its client library are a
# separate product under a different licence.  This target vendors nothing
# from that tree and links nothing from it: daemon_abi.h declares the handful
# of C entry points the adapter uses, and daemon_abi.cc binds them at RUN time
# with dlopen/dlsym against whatever daemon package the operator installed.
# There is deliberately no build-time dependency to add here, and adding one
# would also break the co-install contract -- this module must load and serve
# on a host with no daemon package at all.
#
# Standalone (not folded into the system library) so the startup logic can be
# unit-tested without a server, and so the -ldl link requirement stays scoped
# to the ports that actually run a startup check.  Only the Apache library
# depends on this; the enum the serving seams read lives in daemon_health.h,
# inside the system library, precisely so that depending on the VERDICT does
# not mean depending on the BINDING.
# =============================================================================
cc_library(
    name = "daemon_adapter",
    srcs = [
        "daemon_abi.cc",
        "daemon_adapter.cc",
    ],
    hdrs = [
        "daemon_abi.h",
        "daemon_adapter.h",
    ],
    # daemon_health.h is NOT here -- see the :daemon_health target above.
    linkopts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-ldl"],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":daemon_health",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# The record arm: the gate that decides what may go into the optimizer
# daemon's shared cache for one origin response, and the writer that carries
# that decision out.
#
# Split from :daemon_adapter (startup) rather than folded into it, because the
# gate is a pure function and stays testable without a cache, a socket or a
# server -- and because the startup half must remain reachable from a port
# whose serving seam has not been swapped yet.
#
# The same LICENSING BOUNDARY as :daemon_adapter applies: nothing from the
# daemon's source tree is vendored and nothing from it is linked; every peer
# call goes through the run-time binding in daemon_abi.h.
# =============================================================================
cc_library(
    name = "daemon_record_arm",
    srcs = [
        "daemon_ipro_recorder.cc",
        "daemon_record_arm.cc",
    ],
    hdrs = [
        "daemon_ipro_recorder.h",
        "daemon_record_arm.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":daemon_adapter",
        ":ipro_recorder",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
    ],
)

# =============================================================================
# The serve arm: which stored entry this module may hand back for one
# request, and the response state that goes with it.
#
# Split from :daemon_record_arm for the same reason that one is split from
# :daemon_adapter -- the selection rules and the validator/`Vary`/
# `Cache-Control` composition are pure functions and stay testable without a
# cache, a socket or a server. It depends on the record arm rather than
# duplicating it because both arms apply the SAME two URL restrictions, and
# two copies of "which URLs name an entry we can be sure of" is exactly the
# drift that puts a write where no read looks.
#
# The same LICENSING BOUNDARY as :daemon_adapter applies.
# =============================================================================
cc_library(
    name = "daemon_serve_arm",
    srcs = ["daemon_serve_arm.cc"],
    hdrs = ["daemon_serve_arm.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":daemon_adapter",
        ":daemon_record_arm",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
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
# Excludes memcached_cache.cc (Redis is the preferred external cache).
# This reduces binary size and external dependencies.
cc_library(
    name = "system_envoy",
    srcs = SYSTEM_CORE_SRCS + SYSTEM_GENERATED_SRCS,
    hdrs = SYSTEM_CORE_HDRS,
    copts = [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
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
#   - memcached_cache.cc (libmemcached is POSIX-only)
# Use this target for Windows IIS integration.
cc_library(
    name = "system_windows",
    srcs = SYSTEM_CORE_SRCS + SYSTEM_GENERATED_SRCS,
    hdrs = SYSTEM_CORE_HDRS,
    copts = [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
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
# Includes memcached_cache.cc which is excluded from system_envoy and
# system_windows.
# Generated via pagespeed_system_library() macro in system.bzl.
pagespeed_system_library(name = "system")

# Apache-specific variant (same sources; kept as a distinct target so the
# Apache and nginx link graphs stay independently addressable).
pagespeed_system_library(name = "system_apache")


cc_library(
    name = "curl_fetcher",
    srcs = [
        "curl_fetch.cc",
        "curl_url_async_fetcher.cc",
        "uds_daemon_reader.cc",
    ],
    hdrs = [
        "curl_fetch.h",
        "curl_url_async_fetcher.h",
        "uds_daemon_reader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":daemon_reader",
        "//net/instaweb/http",
        "//net/instaweb:net_instaweb_lib",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "//bazel:curl",
    ],
)

# System-level test scripts for Apache/Nginx system tests
filegroup(
    name = "system_test_files",
    srcs = [
        "system_test.sh",
    ] + glob(["system_tests/*.sh"]),
    visibility = ["//visibility:public"],
)

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Bazel macro for generating the full (memcached-capable) system library targets."""

def pagespeed_system_library(name, **kwargs):
    """Generate a system library target for a POSIX server port.

    This creates a cc_library equivalent to the main `system` target. Used to
    produce the port-specific variants (nginx, apache) without duplicating
    BUILD logic.

    Args:
        name: Target name.
        **kwargs: Additional arguments passed to cc_library.
    """
    native.cc_library(
        name = name,
        srcs = SYSTEM_CORE_SRCS + SYSTEM_GENERATED_SRCS + select({
            "@platforms//os:linux": ["memcached_cache.cc"],
            "//conditions:default": [],
        }),
        hdrs = SYSTEM_CORE_HDRS + select({
            "@platforms//os:linux": ["memcached_cache.h"],
            "//conditions:default": [],
        }),
        copts = [
            "-Wno-trigraphs",  # Generated html_admin_console.cc contains ??X sequences
        ] + select({
            "@platforms//os:linux": [],
            "//conditions:default": ["-DPAGESPEED_ENABLE_MEMCACHED=0"],
        }),
        visibility = ["//visibility:public"],
        deps = SYSTEM_CORE_DEPS + select({
            "@platforms//os:linux": [
                ":memcached_cache",
                "//bazel:libmemcached",
            ],
            "//conditions:default": [],
        }),
        **kwargs
    )

# Canonical definitions of the shared file/dep lists.
# These are loaded by both the macro above and the BUILD file.
SYSTEM_CORE_SRCS = [
    "add_headers_fetcher.cc",
    "admin_daemon_handler.cc",
    "admin_site.cc",
    "external_server_spec.cc",
    "in_place_resource_recorder.cc",
    "ipro_record_gate.cc",
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

SYSTEM_GENERATED_SRCS = [
    ":html_admin_console",
]

SYSTEM_CORE_HDRS = [
    "add_headers_fetcher.h",
    "admin_daemon_handler.h",
    "admin_site.h",
    "external_server_spec.h",
    "in_place_resource_recorder.h",
    "ipro_record_gate.h",
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

SYSTEM_CORE_DEPS = [
    ":daemon_health",
    ":daemon_reader",
    ":ipro_recorder",
    ":external_server_spec",
    ":optimization_thread_policy",
    ":redis_cache",
    "//net/instaweb/http",
    "//net/instaweb/rewriter",
    "//pagespeed/kernel/cache:cyclone_cache",
    "//pagespeed/kernel/sharedmem",
    "//pagespeed/kernel/util",
    "//third_party/redis-crc",
    "@hiredis",
]

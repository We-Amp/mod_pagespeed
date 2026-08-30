"""Bazel macro for generating system library targets with parameterized server type."""

def pagespeed_system_library(name, server_type, **kwargs):
    """Generate a system library target for a specific server port.

    This creates a cc_library equivalent to the main `system` target but with
    a configurable PAGESPEED_SERVER define. Used to produce port-specific
    variants (nginx, apache) without duplicating BUILD logic.

    Args:
        name: Target name.
        server_type: Value for PAGESPEED_SERVER (e.g. "nginx", "apache").
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
            '-DPAGESPEED_SERVER=\\"%s\\"' % server_type,
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
    "admin_license_handler.cc",
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
    "admin_license_handler.h",
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
    ":json_utils",
    ":optimization_thread_policy",
    ":over_cap_match",
    ":redis_cache",
    "//net/instaweb/http",
    "//net/instaweb/rewriter",
    "//pagespeed/kernel/cache:cyclone_cache",
    "//pagespeed/kernel/license_v2:license_file",
    "//pagespeed/kernel/license_v2:license_token",
    "//pagespeed/kernel/license_v2:license_verifier",
    "//pagespeed/kernel/license_v2:tracking_metadata",
    "//pagespeed/kernel/sharedmem",
    "//pagespeed/kernel/util",
    "//third_party/redis-crc",
    "@hiredis",
]

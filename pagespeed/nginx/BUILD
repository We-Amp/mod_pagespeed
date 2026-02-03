load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

licenses(["notice"])  # Apache 2

# =============================================================================
# NGINX PageSpeed Module
# =============================================================================
# This module provides PageSpeed optimization for NGINX servers.
# Build with: bazel build --config=clang-libstdcxx13 //pagespeed/nginx:nginx_pagespeed
#
# See NGX_PAGESPEED_INTEGRATION_PLAN.md for detailed integration documentation.

# Common compiler options for NGINX integration code.
# These include NGINX header paths and suppress warnings from NGINX headers.
NGINX_COPTS = [
    "-Wno-unused-parameter",
    "-Wno-old-style-cast",
]

# Core NGINX PageSpeed library
# Contains the main module implementation and all supporting classes.
cc_library(
    name = "nginx_pagespeed",
    srcs = [
        "log_message_handler.cc",
        "ngx_base_fetch.cc",
        "ngx_caching_headers.cc",
        "ngx_event_connection.cc",
        "ngx_fetch.cc",
        "ngx_gzip_setter.cc",
        "ngx_list_iterator.cc",
        "ngx_message_handler.cc",
        "ngx_pagespeed.cc",
        "ngx_rewrite_driver_factory.cc",
        "ngx_rewrite_options.cc",
        "ngx_server_context.cc",
        "ngx_url_async_fetcher.cc",
    ],
    hdrs = [
        "log_message_handler.h",
        "ngx_base_fetch.h",
        "ngx_caching_headers.h",
        "ngx_event_connection.h",
        "ngx_fetch.h",
        "ngx_gzip_setter.h",
        "ngx_list_iterator.h",
        "ngx_message_handler.h",
        "ngx_pagespeed.h",
        "ngx_rewrite_driver_factory.h",
        "ngx_rewrite_options.h",
        "ngx_server_context.h",
        "ngx_url_async_fetcher.h",
    ],
    copts = NGINX_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/automatic",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/sharedmem",
        "//pagespeed/kernel/thread",
        "//pagespeed/system",
        "//pagespeed/system:curl_fetcher",
        "@nginx//:nginx",
    ],
)

# NGINX PageSpeed module as shared library (.so)
# This can be loaded as a dynamic NGINX module via "load_module" directive.
# Build with: bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so
#
# The module exports the ngx_modules symbol that nginx expects when loading
# dynamic modules via dlsym().
cc_binary(
    name = "ngx_pagespeed_module.so",
    srcs = [
        # Module entry point that exports ngx_modules for dynamic loading
        "ngx_pagespeed_module.cc",
    ],
    copts = NGINX_COPTS + [
        # Ensure symbols are visible by default for dlsym()
        "-fvisibility=default",
    ],
    linkopts = [
        # Export all symbols (needed for nginx to find ngx_modules via dlsym)
        "-Wl,--export-dynamic",
    ],
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":nginx_pagespeed"],
)

# Alternative: Lighter weight version using system_envoy (no memcached, no fork)
# Use this for simpler deployments that only need Redis as external cache.
cc_library(
    name = "nginx_pagespeed_lite",
    srcs = [
        "log_message_handler.cc",
        "ngx_base_fetch.cc",
        "ngx_caching_headers.cc",
        "ngx_event_connection.cc",
        "ngx_fetch.cc",
        "ngx_gzip_setter.cc",
        "ngx_list_iterator.cc",
        "ngx_message_handler.cc",
        "ngx_pagespeed.cc",
        "ngx_rewrite_driver_factory.cc",
        "ngx_rewrite_options.cc",
        "ngx_server_context.cc",
        "ngx_url_async_fetcher.cc",
    ],
    hdrs = [
        "log_message_handler.h",
        "ngx_base_fetch.h",
        "ngx_caching_headers.h",
        "ngx_event_connection.h",
        "ngx_fetch.h",
        "ngx_gzip_setter.h",
        "ngx_list_iterator.h",
        "ngx_message_handler.h",
        "ngx_pagespeed.h",
        "ngx_rewrite_driver_factory.h",
        "ngx_rewrite_options.h",
        "ngx_server_context.h",
        "ngx_url_async_fetcher.h",
    ],
    copts = NGINX_COPTS + [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
        "-DPAGESPEED_NO_FORK",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/automatic",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/sharedmem",
        "//pagespeed/kernel/thread",
        "//pagespeed/system:system_envoy",
        "//pagespeed/system:curl_fetcher",
        "@nginx//:nginx",
    ],
)

# Lite version as shared library
cc_binary(
    name = "ngx_pagespeed_module_lite.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":nginx_pagespeed_lite"],
)

# =============================================================================
# Static archive for linking with nginx configure
# =============================================================================
# This creates a static archive (.a) that can be used with nginx's native
# build system via --add-module. Use this when you want to compile nginx
# from source with PageSpeed built in.
#
# Build with: bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_archive
#
# The archive will be at: bazel-bin/pagespeed/nginx/libngx_pagespeed_archive.a

cc_library(
    name = "ngx_pagespeed_archive",
    # Include all the nginx module sources
    srcs = [
        "log_message_handler.cc",
        "ngx_base_fetch.cc",
        "ngx_caching_headers.cc",
        "ngx_event_connection.cc",
        "ngx_fetch.cc",
        "ngx_gzip_setter.cc",
        "ngx_list_iterator.cc",
        "ngx_message_handler.cc",
        "ngx_pagespeed.cc",
        "ngx_rewrite_driver_factory.cc",
        "ngx_rewrite_options.cc",
        "ngx_server_context.cc",
        "ngx_url_async_fetcher.cc",
    ],
    hdrs = [
        "log_message_handler.h",
        "ngx_base_fetch.h",
        "ngx_caching_headers.h",
        "ngx_event_connection.h",
        "ngx_fetch.h",
        "ngx_gzip_setter.h",
        "ngx_list_iterator.h",
        "ngx_message_handler.h",
        "ngx_pagespeed.h",
        "ngx_rewrite_driver_factory.h",
        "ngx_rewrite_options.h",
        "ngx_server_context.h",
        "ngx_url_async_fetcher.h",
    ],
    copts = NGINX_COPTS,
    # Force static linking to create a self-contained archive
    linkstatic = True,
    visibility = ["//visibility:public"],
    # alwayslink ensures all symbols are included even if not directly referenced
    alwayslink = True,
    deps = [
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/automatic",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/sharedmem",
        "//pagespeed/kernel/thread",
        "//pagespeed/system",
        "//pagespeed/system:curl_fetcher",
        "@nginx//:nginx",
    ],
)

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

# Segment queue used by NgxBaseFetch to buffer response bytes between PSOL
# threads and the nginx thread (M0 of the zero-copy serving ladder).
# Deliberately nginx-free so it can be unit tested standalone
# (//test/pagespeed/nginx:ngx_segment_buffer_test). The module targets below
# compile ngx_segment_buffer.cc directly in their srcs instead of depending on
# this target, because the --add-module archive (ngx_pagespeed_archive, an
# alwayslink .lo) only bundles its own srcs — a dep would leave undefined
# symbols at nginx's own link step.
cc_library(
    name = "ngx_segment_buffer",
    srcs = ["ngx_segment_buffer.cc"],
    hdrs = ["ngx_segment_buffer.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

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
        "ngx_openssl_shim.cc",
        "ngx_gzip_setter.cc",
        "ngx_list_iterator.cc",
        "ngx_message_handler.cc",
        "ngx_pagespeed.cc",
        "ngx_rewrite_driver_factory.cc",
        "ngx_rewrite_options.cc",
        "ngx_segment_buffer.cc",
        "ngx_server_context.cc",
        "ngx_url_async_fetcher.cc",
        "ngx_webbotauth_handler.cc",
    ],
    hdrs = [
        "log_message_handler.h",
        "ngx_base_fetch.h",
        "ngx_caching_headers.h",
        "ngx_event_connection.h",
        "ngx_fetch.h",
        "ngx_openssl_shim.h",
        "ngx_gzip_setter.h",
        "ngx_list_iterator.h",
        "ngx_message_handler.h",
        "ngx_pagespeed.h",
        "ngx_rewrite_driver_factory.h",
        "ngx_rewrite_options.h",
        "ngx_segment_buffer.h",
        "ngx_server_context.h",
        "ngx_url_async_fetcher.h",
        "ngx_webbotauth_handler.h",
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
        "//pagespeed/kernel/webbotauth:webbotauth_core",
        "//pagespeed/kernel/webbotauth:webbotauth_counter",
        "//pagespeed/kernel/webbotauth:webbotauth_warmer",
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
        # Hide all symbols by default; the version script controls exports
        "-fvisibility=hidden",
    ],
    linkopts = [
        # Version script: export only ngx_modules/ngx_module_names/ngx_module_order.
        # Hides BoringSSL/curl/internal symbols to avoid conflicts with nginx's
        # own libssl (system OpenSSL) loaded in the same process.
        "-Wl,--version-script,$(location :ngx_pagespeed.lds)",
    ],
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        ":nginx_pagespeed",
        ":ngx_pagespeed.lds",
    ],
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
        "ngx_openssl_shim.cc",
        "ngx_gzip_setter.cc",
        "ngx_list_iterator.cc",
        "ngx_message_handler.cc",
        "ngx_pagespeed.cc",
        "ngx_rewrite_driver_factory.cc",
        "ngx_rewrite_options.cc",
        "ngx_segment_buffer.cc",
        "ngx_server_context.cc",
        "ngx_url_async_fetcher.cc",
        "ngx_webbotauth_handler.cc",
    ],
    hdrs = [
        "log_message_handler.h",
        "ngx_base_fetch.h",
        "ngx_caching_headers.h",
        "ngx_event_connection.h",
        "ngx_fetch.h",
        "ngx_openssl_shim.h",
        "ngx_gzip_setter.h",
        "ngx_list_iterator.h",
        "ngx_message_handler.h",
        "ngx_pagespeed.h",
        "ngx_rewrite_driver_factory.h",
        "ngx_rewrite_options.h",
        "ngx_segment_buffer.h",
        "ngx_server_context.h",
        "ngx_url_async_fetcher.h",
        "ngx_webbotauth_handler.h",
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
        "//pagespeed/kernel/webbotauth:webbotauth_core",
        "//pagespeed/kernel/webbotauth:webbotauth_counter",
        "//pagespeed/kernel/webbotauth:webbotauth_warmer",
        "@nginx//:nginx",
    ],
)

# Lite version as shared library
cc_binary(
    name = "ngx_pagespeed_module_lite.so",
    srcs = [
        "ngx_pagespeed_module.cc",
    ],
    copts = NGINX_COPTS + [
        "-fvisibility=hidden",
    ],
    linkopts = [
        "-Wl,--version-script,$(location :ngx_pagespeed.lds)",
    ],
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        ":nginx_pagespeed_lite",
        ":ngx_pagespeed.lds",
    ],
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
    # All the nginx-module support sources EXCEPT ngx_pagespeed.cc.
    #
    # ngx_pagespeed.cc carries the `ngx_module_t ngx_pagespeed` definition and
    # the module entry points, and it is compiled by nginx's OWN build via
    # --add-dynamic-module (pagespeed/nginx/config lists it in NGX_PAGESPEED_SRCS).
    # If the archive ALSO contained ngx_pagespeed.cc, the final module link would
    # pull in two copies of every symbol it defines (process_context, the
    # ngx_pagespeed struct, str_to_string_piece, ...) and fail with "multiple
    # definition". So the archive provides only the supporting translation units;
    # nginx compiles ngx_pagespeed.cc itself and links it against this archive.
    srcs = [
        "log_message_handler.cc",
        "ngx_base_fetch.cc",
        "ngx_caching_headers.cc",
        "ngx_event_connection.cc",
        "ngx_fetch.cc",
        "ngx_openssl_shim.cc",
        "ngx_gzip_setter.cc",
        "ngx_list_iterator.cc",
        "ngx_message_handler.cc",
        "ngx_rewrite_driver_factory.cc",
        "ngx_rewrite_options.cc",
        "ngx_segment_buffer.cc",
        "ngx_server_context.cc",
        "ngx_url_async_fetcher.cc",
        "ngx_webbotauth_handler.cc",
    ],
    hdrs = [
        "log_message_handler.h",
        "ngx_base_fetch.h",
        "ngx_caching_headers.h",
        "ngx_event_connection.h",
        "ngx_fetch.h",
        "ngx_openssl_shim.h",
        "ngx_gzip_setter.h",
        "ngx_list_iterator.h",
        "ngx_message_handler.h",
        "ngx_pagespeed.h",
        "ngx_rewrite_driver_factory.h",
        "ngx_rewrite_options.h",
        "ngx_segment_buffer.h",
        "ngx_server_context.h",
        "ngx_url_async_fetcher.h",
        "ngx_webbotauth_handler.h",
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
        "//pagespeed/kernel/webbotauth:webbotauth_core",
        "//pagespeed/kernel/webbotauth:webbotauth_counter",
        "//pagespeed/kernel/webbotauth:webbotauth_warmer",
        "@nginx//:nginx",
    ],
)

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

licenses(["notice"])  # Apache 2

# =============================================================================
# IIS Native Module for PageSpeed
# =============================================================================
# This module provides direct C++ integration with IIS (no C ABI boundary).
# Build with: bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis

# =============================================================================
# Platform-independent utilities (can be tested on Linux)
# =============================================================================

# HTTP chunked transfer encoding decoder
# This is platform-independent and can be built/tested on any platform.
cc_library(
    name = "iis_dechunker",
    srcs = ["iis_dechunker.cc"],
    hdrs = ["iis_dechunker.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Content decoder for gzip/deflate
# This is platform-independent and can be built/tested on any platform.
cc_library(
    name = "iis_content_decoder",
    srcs = ["iis_content_decoder.cc"],
    hdrs = ["iis_content_decoder.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@envoy//bazel:zlib",
    ],
)

# Streaming fetch for HTML flushing support
# Platform-independent version for testing on Linux (uses mock IIS)
# The real implementation is included in iis_module for Windows.
cc_library(
    name = "iis_streaming_fetch",
    srcs = ["iis_streaming_fetch.cc"],
    hdrs = [
        "iis_streaming_fetch.h",
        "iis_flush_notification.h",
    ],
    testonly = True,  # Uses mock_iis which is testonly
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//test/pagespeed/iis:mock_iis",
    ],
)

# =============================================================================
# IIS-specific module (Windows only)
# =============================================================================

# Core IIS module library
cc_library(
    name = "iis_module",
    srcs = [
        "iis_admin_handler.cc",
        "iis_async_fetch.cc",
        "iis_base_fetch.cc",
        "iis_beacon_handler.cc",
        "iis_config.cc",
        "iis_content_decoder.cc",
        "iis_dechunker.cc",
        "iis_event_log.cc",
        "iis_http_module.cc",
        "iis_inplace_resource_handler.cc",
        "iis_message_handler.cc",
        "iis_module_factory.cc",
        "iis_perf_counters.cc",
        "iis_rewrite_driver_factory.cc",
        "iis_server_context.cc",
        "iis_streaming_fetch.cc",
        "iis_utils.cc",
        "license_validator.cc",
    ],
    hdrs = [
        "iis_admin_handler.h",
        "iis_async_fetch.h",
        "iis_base_fetch.h",
        "iis_beacon_handler.h",
        "iis_config.h",
        "iis_content_decoder.h",
        "iis_dechunker.h",
        "iis_event_log.h",
        "iis_flush_notification.h",
        "iis_http_module.h",
        "iis_inplace_resource_handler.h",
        "iis_message_handler.h",
        "iis_module_factory.h",
        "iis_perf_counters.h",
        "iis_rewrite_driver_factory.h",
        "iis_server_context.h",
        "iis_streaming_fetch.h",
        "iis_utils.h",
        "license_validator.h",
    ],
    defines = [
        "PAGESPEED_IIS_MODULE",
    ],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/automatic",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/image",
        "//pagespeed/kernel/sharedmem",
        "//pagespeed/system:curl_fetcher",
        "//pagespeed/system:system_windows",
        "@envoy//bazel:zlib",  # For content decompression (gzip/deflate)
    ],
)

# IIS module as shared library (DLL)
# Uses cc_binary with linkshared=True to produce pagespeed_iis.dll
cc_binary(
    name = "pagespeed_iis.dll",
    srcs = ["dll_main.cc"],
    defines = ["PAGESPEED_DLL_EXPORT"],
    linkshared = True,
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [":iis_module"],
    # Export RegisterModule for IIS native module loading
    linkopts = select({
        "@platforms//os:windows": ["/EXPORT:RegisterModule"],
        "//conditions:default": [],
    }),
)

# For now, also provide a static library target for testing
cc_library(
    name = "iis_module_static",
    visibility = ["//visibility:public"],
    deps = [":iis_module"],
)

# Performance counter manifest for Windows perfmon.exe integration
# See iis_perf_counters.cc for installation instructions
filegroup(
    name = "perf_counter_manifest",
    srcs = ["pagespeed_counters.man"],
    visibility = ["//visibility:public"],
)

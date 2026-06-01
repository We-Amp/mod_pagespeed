load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

licenses(["notice"])  # Apache 2

# =============================================================================
# IIS Native Module for PageSpeed — IISpeed adoption
# =============================================================================
# Build with: bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis

# =============================================================================
# Core IIS module library (IISpeed-based)
# =============================================================================

cc_library(
    name = "iis_module",
    srcs = [
        # IISpeed module sources
        "iis_http_module.cpp",
        "iis_module_factory.cpp",
        "iis_module_request_context.cpp",
        "iis_module_base_fetch.cpp",
        "iis_module_misc.cpp",
        # IISpeed shared sources
        "iis_process_context.cpp",
        "iis_request_context.cpp",
        "iis_psol_request_context.cpp",
        "iis_server_context.cpp",
        "iis_message_handler.cpp",
        "log_message_handler.cpp",
        "iis_rewrite_driver_factory.cpp",
        "iis_rewrite_options.cpp",
        "iis_configuration.cpp",
        "iis_global_constants.cpp",
        "iis_base_fetch.cpp",
        "iis_async_url_fetcher.cpp",
        "asyncwinhttp.cpp",
        "iis_misc.cpp",
        # IISpeed utilities
        "util.cpp",
        "FileCheck.cpp",
    ],
    hdrs = [
        # IISpeed headers
        "iis_http_module.h",
        "iis_module_factory.h",
        "iis_module_request_context.h",
        "iis_module_base_fetch.h",
        "iis_module_misc.h",
        "iis_process_context.h",
        "iis_request_context.h",
        "iis_psol_request_context.h",
        "iis_server_context.h",
        "iis_message_handler.h",
        "log_message_handler.h",
        "iis_rewrite_driver_factory.h",
        "iis_rewrite_options.h",
        "iis_configuration.h",
        "iis_global_constants.h",
        "iis_base_fetch.h",
        "iis_async_url_fetcher.h",
        "asyncwinhttp.h",
        "dechunker.h",
        "iis_misc.h",
        "iis_debugging_string_writer.h",
        "util.h",
        "FileCheck.h",
    ],
    defines = [
        "PAGESPEED_IIS_MODULE",
        "_WINSOCKAPI_",
    ],
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb:net_instaweb_lib",
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/automatic",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/image",
        "//pagespeed/kernel/sharedmem",
        "//pagespeed/kernel/thread:win_thread",
        "//pagespeed/system:system_windows",
        "@com_googlesource_code_re2//:re2",
        "@zlib_ng//:zlib_ng",
    ],
    linkopts = select({
        "@platforms//os:windows": [
            "winhttp.lib",
            "advapi32.lib",
            "shlwapi.lib",
            "ws2_32.lib",
            "bcrypt.lib",
        ],
        "//conditions:default": [],
    }),
)

# IIS module as shared library (DLL)
cc_binary(
    name = "pagespeed_iis.dll",
    srcs = ["dll_main.cc"],
    defines = ["PAGESPEED_DLL_EXPORT"],
    linkshared = True,
    target_compatible_with = ["@platforms//os:windows"],
    visibility = ["//visibility:public"],
    deps = [":iis_module"],
    linkopts = select({
        "@platforms//os:windows": ["/EXPORT:RegisterModule"],
        "//conditions:default": [],
    }),
)

# Performance counter manifest for Windows perfmon.exe integration
filegroup(
    name = "perf_counter_manifest",
    srcs = ["pagespeed_counters.man"],
    visibility = ["//visibility:public"],
)

# Platform-independent dechunker (header-only).
# Can be tested on any platform.
cc_library(
    name = "iis_dechunker",
    hdrs = ["dechunker.h"],
    visibility = ["//visibility:public"],
)

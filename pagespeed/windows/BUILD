load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

# IIS-specific RewriteDriverFactory subclass.
# These targets are for Windows IIS integration.
# Build with: bazel build --config=windows --config=clang-cl //pagespeed/windows:iis_rewrite_driver_factory
cc_library(
    name = "iis_rewrite_driver_factory",
    srcs = ["iis_rewrite_driver_factory.cc"],
    hdrs = ["iis_rewrite_driver_factory.h"],
    tags = ["manual"],  # Excluded from //pagespeed/... builds
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/system:curl_fetcher",
    ] + select({
        "@platforms//os:windows": ["//pagespeed/system:system_windows"],
        "//conditions:default": ["//pagespeed/system"],
    }),
)

# C API boundary for pagespeed_automatic on Windows.
# This produces a DLL-safe interface consumable by IIS native modules.
# Build with: bazel build --config=zig-windows //pagespeed/windows:pagespeed_c_api
cc_library(
    name = "pagespeed_c_api",
    srcs = ["pagespeed_c_api.cc"],
    hdrs = ["pagespeed_c_api.h"],
    defines = ["PAGESPEED_DLL_EXPORT"],
    tags = ["manual"],  # Excluded from //pagespeed/... builds
    visibility = ["//visibility:public"],
    deps = [
        ":iis_rewrite_driver_factory",
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
    ] + select({
        "@platforms//os:windows": ["//pagespeed/system:system_windows"],
        "//conditions:default": ["//pagespeed/system"],
    }),
)

load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

cc_library(
    name = "sharedmem",
    srcs = [
        "inprocess_shared_mem.cc",
        "shared_circular_buffer.cc",
        "shared_dynamic_string_map.cc",
        "shared_mem_cache.cc",
        "shared_mem_cache_data.cc",
        "shared_mem_lock_manager.cc",
        "shared_mem_statistics.cc",
    ] + select({
        "@platforms//os:windows": ["windows_shared_mem.cc"],
        "//conditions:default": [],
    }),
    hdrs = [
        "inprocess_shared_mem.h",
        "shared_circular_buffer.h",
        "shared_dynamic_string_map.h",
        "shared_mem_cache.h",
        "shared_mem_cache_data.h",
        "shared_mem_lock_manager.h",
        "shared_mem_statistics.h",
    ] + select({
        "@platforms//os:windows": ["windows_shared_mem.h"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/util",
    ],
)

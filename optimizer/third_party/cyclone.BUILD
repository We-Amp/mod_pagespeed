# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Cyclone Cache - High-performance disk cache library
# Requires C++23

cc_library(
    name = "cyclone",
    srcs = [
        "src/c_api/cyclone_c.cpp",
        "src/core/cache.cpp",
        "src/core/crc32c.cpp",
        "src/core/directory.cpp",
        "src/core/document.cpp",
        "src/core/hit_tracker.cpp",
        "src/core/key.cpp",
        "src/core/mmap_directory.cpp",
        "src/core/volume.cpp",
        "src/core/write_buffer.cpp",
        "src/io/mapped_file.cpp",
        "src/optimization/adaptive_pool.cpp",
        "src/optimization/load_monitor.cpp",
        "src/optimization/optimization_engine.cpp",
        "src/optimization/work_queue.cpp",
        "src/plugin/http/http_alternate.cpp",
        "src/plugin/http/http_metadata.cpp",
        "src/plugin/plugin_manager.cpp",
        "src/ram_cache/clfus.cpp",
        "src/ram_cache/lru.cpp",
    ],
    hdrs = glob([
        "include/**/*.h",
        "include/**/*.hpp",
        "src/**/*.hpp",
    ]),
    copts = select({
        "@platforms//os:windows": [],
        # Upstream cyclone uses callback-style APIs with unused parameters
        "//conditions:default": ["-Wno-unused-parameter"],
    }),
    defines = [
        "CYCLONE_USE_BUNDLED_SHA256",
        "CYCLONE_HTTP_PLUGIN",
    ] + select({
        "@platforms//os:macos": ["CYCLONE_PLATFORM_MACOS"],
        "@platforms//os:linux": ["CYCLONE_PLATFORM_LINUX"],
        "@platforms//os:windows": ["CYCLONE_PLATFORM_WINDOWS"],
        "//conditions:default": [],
    }),
    includes = [
        "include",
        "src",
        "src/c_api",
        "src/core",
        "src/io",
        "src/optimization",
        "src/plugin",
        "src/plugin/http",
        "src/ram_cache",
    ],
    linkopts = select({
        "@platforms//os:linux": ["-lpthread"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

# C API only library (for Nginx module which is pure C)
cc_library(
    name = "cyclone_c",
    hdrs = ["include/cyclone/cyclone_c.h"],
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [":cyclone"],
)

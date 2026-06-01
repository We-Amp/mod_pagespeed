# Build rules for Cyclone Cache library
#
# Cyclone is a high-performance disk cache with scan-resistant CLFUS algorithm,
# memory-mapped I/O, and built-in RAM cache. It requires C++23.
#
# This file defines the build configuration for Cyclone as an external Bazel
# dependency. The C-style wrapper that bridges C++23 to C++20 is defined
# separately in //pagespeed/kernel/cache/cyclone:cyclone_wrapper.

cyclone_build_rule = """
load("@rules_cc//cc:defs.bzl", "cc_library")

# Core Cyclone library (C++23)
# This library contains the actual cache implementation.
#
# IMPORTANT: Cyclone requires C++23. The per_file_copt in pagespeed.bazelrc
# overrides the default standard for cyclone source files. We also include
# -std=c++23 in copts as a fallback.
cc_library(
    name = "cyclone",
    srcs = glob([
        "src/core/*.cpp",
        "src/io/*.cpp",
        "src/ram_cache/*.cpp",
        "src/plugin/*.cpp",
        "src/optimization/*.cpp",
    ], exclude = [
        "src/**/*_test.cpp",
        "src/**/*_benchmark.cpp",
    ]),
    hdrs = glob([
        "include/cyclone/*.hpp",
        "include/cyclone/detail/*.hpp",
        "include/cyclone/plugin/*.hpp",
        "src/core/*.hpp",
        "src/io/*.hpp",
        "src/ram_cache/*.hpp",
        "src/optimization/*.hpp",
    ]),
    includes = ["include", "src"],
    copts = select({
        "@platforms//os:windows": [
            "/std:c++latest",
            "/FImutex",
            "/FIshared_mutex",
            "/FIexpected",
            "/FIalgorithm",  # For std::clamp
        ],
        "//conditions:default": [
            "-std=c++23",
            "-Wno-deprecated-declarations",
            # Force-include standard library headers that cyclone relies on via
            # indirect includes. The Bazel build order differs from CMake.
            "-include", "mutex",
            "-include", "shared_mutex",
            "-include", "expected",
        ],
    }),
    local_defines = select({
        "@platforms//os:windows": [
            "CYCLONE_PLATFORM_WINDOWS",
            # ssize_t is POSIX, define it for Windows
            "ssize_t=__int64",
            # TODO: Cyclone has Windows compatibility issues with struct stat
            # The _fstat function expects struct _stat64i32, but Cyclone uses struct stat
            # This requires fixing in the Cyclone source code itself
        ],
        "//conditions:default": [],
    }),
    linkopts = select({
        "@platforms//os:linux": ["-lpthread"],
        "//conditions:default": [],
    }),
    deps = [
        # BoringSSL provides OpenSSL-compatible API for SHA-256
        "@boringssl//:crypto",
    ],
    visibility = ["//visibility:public"],
)
"""


load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

# =============================================================================
# Cyclone Cache C ABI Wrapper
# =============================================================================
#
# This directory provides a C ABI wrapper for the Cyclone cache library,
# enabling C++20 code (the main mod_pagespeed codebase) to use the C++23
# Cyclone library without requiring the entire codebase to be compiled
# with C++23.
#
# ARCHITECTURE:
# ~~~~~~~~~~~~
#   +-------------------+     +-------------------+     +-------------------+
#   | C++20 Code        |     | C ABI Wrapper     |     | Cyclone Library   |
#   | (mod_pagespeed)   | --> | (cyclone_wrapper) | --> | (C++23)           |
#   | Uses C header     |     | C++23 impl        |     | std::expected etc |
#   +-------------------+     +-------------------+     +-------------------+
#
# FILES:
# ~~~~~~
#   - cyclone_wrapper.h  - C header with C types only, safe for C++20
#   - cyclone_wrapper.cc - C++23 implementation that bridges to Cyclone
#
# BUILD REQUIREMENTS:
# ~~~~~~~~~~~~~~~~~~
# Cyclone requires C++23 for std::expected. Use --config=gcc to build with
# GCC 13 which has full C++23 support. Clang 17 with libstdc++ may have
# issues with std::expected availability.
#
#   # Build with GCC 13 (required for C++23 std::expected):
#   docker compose exec dev bazel build --config=gcc //...
#
# USAGE:
# ~~~~~~
# To use Cyclone from C++20 code:
#
#   1. Depend on //pagespeed/kernel/cache:cyclone_cache (NOT this wrapper)
#   2. The cyclone_cache library wraps this C ABI in a C++ CacheInterface
#   3. The C ABI functions (cyclone_cache_*, cyclone_read_handle_*) are
#      exposed via cyclone_wrapper.h for direct use if needed
#
# See cyclone_wrapper.h for the complete C API documentation.
# =============================================================================

# C-style wrapper header for Cyclone Cache.
# This header uses only C types and can be safely included from C++20 code.
cc_library(
    name = "cyclone_wrapper_hdrs",
    hdrs = ["cyclone_wrapper.h"],
    visibility = ["//pagespeed/kernel/cache:__pkg__"],
)

# C++23 implementation of the Cyclone wrapper.
# This library bridges the C ABI to the C++23 Cyclone library.
# The wrapper isolates C++23 code so that C++20 code can link against it.
#
# IMPORTANT: This target MUST be compiled with C++23. The copts below
# set -std=c++23 explicitly. When using --config=gcc, GCC 13 provides
# full C++23 support including std::expected.
cc_library(
    name = "cyclone_wrapper",
    srcs = ["cyclone_wrapper.cc"],
    copts = [
        "-std=c++23",
        "-Wno-deprecated-declarations",
        # Force-include <expected> as cyclone headers use std::expected
        # but don't always include the header directly.
        "-include",
        "expected",
    ],
    visibility = ["//pagespeed/kernel/cache:__pkg__"],
    deps = [
        ":cyclone_wrapper_hdrs",
        "@cyclone//:cyclone",
    ],
)

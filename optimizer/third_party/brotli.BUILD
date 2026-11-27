# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "brotlicommon",
    srcs = glob(["c/common/*.c"]),
    hdrs = glob([
        "c/common/*.h",
        "c/include/brotli/*.h",
    ]),
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            # Upstream brotli uses mixed signed/unsigned comparisons
            "-Wno-sign-compare",
        ],
    }),
    includes = ["c/include"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "brotlienc",
    srcs = glob([
        "c/enc/*.c",
        "c/enc/*.h",
    ]),
    hdrs = ["c/include/brotli/encode.h"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            # Upstream brotli uses mixed signed/unsigned comparisons
            "-Wno-sign-compare",
        ],
    }),
    includes = ["c/include"],
    visibility = ["//visibility:public"],
    deps = [":brotlicommon"],
)

cc_library(
    name = "brotlidec",
    srcs = glob([
        "c/dec/*.c",
        "c/dec/*.h",
    ]),
    hdrs = ["c/include/brotli/decode.h"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            # Upstream brotli uses mixed signed/unsigned comparisons
            "-Wno-sign-compare",
        ],
    }),
    includes = ["c/include"],
    visibility = ["//visibility:public"],
    deps = [":brotlicommon"],
)

# Convenience target that provides both encoder and decoder.
cc_library(
    name = "brotli",
    visibility = ["//visibility:public"],
    deps = [
        ":brotlidec",
        ":brotlienc",
    ],
)

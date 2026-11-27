# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libyuv - YUV conversion and scaling library (Google)
#
# Used by libavif for image scaling and RGB↔YUV conversion.
# Arch-specific SIMD code (NEON, SSE, etc.) is guarded by #ifdef
# so all source files can be compiled on any platform.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "libyuv",
    srcs = glob(
        ["source/*.cc"],
        exclude = [
            # Exclude JPEG support — pulls in libjpeg headers that
            # conflict with our libjpeg-turbo setup.
            "source/convert_jpeg.cc",
            "source/mjpeg_decoder.cc",
            "source/mjpeg_validate.cc",
        ],
    ),
    hdrs = glob(["include/**/*.h"]),
    copts = [
        "-DLIBYUV_DISABLE_SVE",
        "-DLIBYUV_DISABLE_SME",
    ] + select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            # Upstream libyuv conversion functions have unused parameters
            "-Wno-unused-parameter",
            # Upstream libyuv uses mixed signed/unsigned comparisons
            "-Wno-sign-compare",
        ],
    }) + select({
        # ARM64 NEON code uses i8mm/dotprod instructions that
        # require explicit arch flags for the assembler.
        "@platforms//cpu:aarch64": ["-march=armv8.2-a+dotprod+i8mm"],
        "//conditions:default": [],
    }),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

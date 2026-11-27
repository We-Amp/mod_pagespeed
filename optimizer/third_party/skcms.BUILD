# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# skcms - Skia's lightweight color management system.
# Used by jpegli CMS for SSIMULACRA2 perceptual quality metric.
# Source: https://skia.googlesource.com/skcms
# License: BSD-3-Clause

# GCC/Clang warning suppression (not valid on MSVC)
SKCMS_COPTS = select({
    "@platforms//os:windows": [],
    "//conditions:default": ["-Wno-unused-parameter"],
})

cc_library(
    name = "skcms_TransformBaseline",
    srcs = [
        "src/skcms_Transform.h",
        "src/skcms_TransformBaseline.cc",
        "src/skcms_internals.h",
        "src/skcms_public.h",
    ],
    copts = SKCMS_COPTS,
    local_defines = ["SKCMS_IMPLEMENTATION=1"],
    textual_hdrs = ["src/Transform_inl.h"],
)

cc_library(
    name = "skcms_TransformHsw",
    srcs = [
        "src/skcms_Transform.h",
        "src/skcms_TransformHsw.cc",
        "src/skcms_internals.h",
        "src/skcms_public.h",
    ],
    # AVX2 flags: GCC/Clang need -mavx2 etc. MSVC ignores unknown -m flags
    # (D9002 warning); skcms falls back to baseline on Windows (acceptable
    # perf trade-off — only used for SSIMULACRA2 color management).
    copts = SKCMS_COPTS + select({
        "@platforms//cpu:x86_64": [
            "-mavx2",
            "-mf16c",
            "-mfma",
        ],
        "//conditions:default": [],
    }),
    local_defines = ["SKCMS_IMPLEMENTATION=1"],
    textual_hdrs = ["src/Transform_inl.h"],
)

cc_library(
    name = "skcms_TransformSkx",
    srcs = [
        "src/skcms_Transform.h",
        "src/skcms_TransformSkx.cc",
        "src/skcms_internals.h",
        "src/skcms_public.h",
    ],
    # AVX-512 flags: GCC/Clang need these. MSVC ignores unknown -m flags
    # (D9002 warning); skcms falls back to baseline on Windows.
    copts = SKCMS_COPTS + select({
        "@platforms//cpu:x86_64": [
            "-mavx512f",
            "-mavx512dq",
            "-mavx512cd",
            "-mavx512bw",
            "-mavx512vl",
        ],
        "//conditions:default": [],
    }),
    local_defines = ["SKCMS_IMPLEMENTATION=1"],
    textual_hdrs = ["src/Transform_inl.h"],
)

cc_library(
    name = "skcms",
    srcs = [
        "skcms.cc",
        "src/skcms_internals.h",
        "src/skcms_public.h",
    ],
    hdrs = ["skcms.h"],
    copts = SKCMS_COPTS,
    includes = ["."],
    local_defines = ["SKCMS_IMPLEMENTATION=1"],
    visibility = ["//visibility:public"],
    deps = [
        ":skcms_TransformBaseline",
        ":skcms_TransformHsw",
        ":skcms_TransformSkx",
    ],
)

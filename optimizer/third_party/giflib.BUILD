# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "giflib_core",
    srcs = [
        "gif_err.c",
        "gifalloc.c",
        "openbsd-reallocarray.c",
    ],
    hdrs = [
        "gif_hash.h",
        "gif_lib.h",
        "gif_lib_private.h",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "dgiflib",
    srcs = [
        "dgif_lib.c",
    ],
    # Upstream giflib passes char* where unsigned char* expected
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-pointer-sign"],
    }),
    defines = [
        "_GBA_NO_FILEIO",
    ],
    visibility = ["//visibility:public"],
    deps = [":giflib_core"],
)

cc_library(
    name = "egiflib",
    srcs = [
        "egif_lib.c",
        "gif_hash.c",
    ],
    # Upstream giflib passes char* where unsigned char* expected
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-pointer-sign"],
    }),
    defines = [
        "_GBA_NO_FILEIO",
        "HAVE_FCNTL_H",
    ],
    visibility = ["//visibility:public"],
    deps = [":giflib_core"],
)

# Combined giflib library
cc_library(
    name = "giflib",
    visibility = ["//visibility:public"],
    deps = [
        ":dgiflib",
        ":egiflib",
        ":giflib_core",
    ],
)

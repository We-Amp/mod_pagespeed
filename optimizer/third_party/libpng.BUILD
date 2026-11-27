# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Generate pnglibconf.h from the prebuilt version in scripts/
genrule(
    name = "pnglibconf",
    srcs = ["scripts/pnglibconf.h.prebuilt"],
    outs = ["pnglibconf.h"],
    cmd = "cp $< $@",
)

cc_library(
    name = "libpng",
    srcs = [
        "png.c",
        "pngerror.c",
        "pngget.c",
        "pngmem.c",
        "pngpread.c",
        "pngread.c",
        "pngrio.c",
        "pngrtran.c",
        "pngrutil.c",
        "pngset.c",
        "pngtrans.c",
        "pngwio.c",
        "pngwrite.c",
        "pngwtran.c",
        "pngwutil.c",
    ] + select({
        "@platforms//cpu:aarch64": [
            "arm/arm_init.c",
            "arm/filter_neon_intrinsics.c",
            "arm/palette_neon_intrinsics.c",
        ],
        "//conditions:default": [],
    }),
    hdrs = [
        "png.h",
        "pngconf.h",
        "pngdebug.h",
        "pnginfo.h",
        "pngpriv.h",
        "pngstruct.h",
        ":pnglibconf",
    ],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            # Upstream libpng conditional code paths leave vars unused
            "-Wno-unused-but-set-variable",
        ],
    }) + select({
        # macOS libpng needs explicit math.h for floor()/ceil()
        "@platforms//os:macos": [
            "-include",
            "math.h",
        ],
        "//conditions:default": [],
    }) + select({
        "@platforms//cpu:aarch64": [
            # UB: NEON intrinsics use misaligned loads, valid on ARM but
            # flagged by UBSan. Upstream libpng issue, not our code.
            "-fno-sanitize=alignment",
        ],
        "//conditions:default": [],
    }),
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = ["@zlib"],
)

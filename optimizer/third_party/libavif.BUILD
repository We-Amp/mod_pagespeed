# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libavif - AVIF image format library (encoder + decoder via libaom)
#
# Both encoder and decoder paths are enabled. The AOM codec backend
# is selected via AVIF_CODEC_AOM / AVIF_CODEC_AOM_ENCODE / _DECODE defines.
# Decoder is needed for quality sweep SSIMULACRA2 verification.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "libavif",
    srcs = [
        "src/alpha.c",
        "src/avif.c",
        "src/codec_aom.c",
        "src/colr.c",
        "src/colrconvert.c",
        "src/diag.c",
        "src/exif.c",
        "src/gainmap.c",
        "src/io.c",
        "src/mem.c",
        "src/obu.c",
        "src/properties.c",
        "src/rawdata.c",
        "src/read.c",
        "src/reformat.c",
        "src/reformat_libsharpyuv.c",
        "src/reformat_libyuv.c",
        "src/scale.c",
        "src/stream.c",
        "src/utils.c",
        "src/write.c",
    ],
    hdrs = [
        "include/avif/avif.h",
        "include/avif/internal.h",
    ],
    copts = [
        "-DAVIF_CODEC_AOM=1",
        "-DAVIF_CODEC_AOM_ENCODE=1",
        # Encode AND decode: the SSIMULACRA2 verify floor decodes its own
        # AVIF candidates via libavif (#1274 — the floor silently no-opped
        # while the aom decode path was compiled out; there is no AVIF
        # input reader in the scanline framework). libavif guards the path
        # with `#if defined(AVIF_CODEC_AOM_DECODE)`. Pairs with
        # CONFIG_AV1_DECODER=1 in libaom.BUILD.
        "-DAVIF_CODEC_AOM_DECODE=1",
        "-DAVIF_LIBYUV_ENABLED=1",
    ] + select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            # Upstream libavif codec interface has unused parameters
            "-Wno-unused-parameter",
            # Upstream libavif uses mixed signed/unsigned comparisons
            "-Wno-sign-compare",
        ],
    }),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = [
        "@libaom",
        "@libyuv",
    ],
)

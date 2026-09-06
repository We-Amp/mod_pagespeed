# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

libwebp_build_rule = """
# Description:
#   WebP codec library

licenses(["notice"])  # WebM license

exports_files(["COPYING"])

# Config setting for x86/x64 platforms where SSE is available
config_setting(
    name = "is_x86",
    constraint_values = ["@platforms//cpu:x86_64"],
)

config_setting(
    name = "is_x86_32",
    constraint_values = ["@platforms//cpu:x86_32"],
)

# SSE intrinsics - only built on x86 platforms
cc_library(
    name = "libwebp_sse",
    srcs = select({
        ":is_x86": glob([
            "src/dsp/*_sse2.c",
            "src/dsp/*_sse41.c",
        ]) + ["sharpyuv/sharpyuv_sse2.c"],
        ":is_x86_32": glob([
            "src/dsp/*_sse2.c",
            "src/dsp/*_sse41.c",
        ]) + ["sharpyuv/sharpyuv_sse2.c"],
        "//conditions:default": [],
    }),
    hdrs = glob([
        "src/dsp/*.h",
        "src/webp/*.h",
        "src/dec/*.h",
        "src/enc/*.h",
        "src/utils/*.h",
        "sharpyuv/*.h",
    ]),
    copts = select({
        ":is_x86": ["-mssse3", "-msse4.1"],
        ":is_x86_32": ["-mssse3", "-msse4.1"],
        "//conditions:default": [],
    }),
    includes = ["."],
    visibility = ["//visibility:private"],
)

# SharpYUV CPU info - compiles cpu.c with VP8GetCPUInfo renamed to
# SharpYuvGetCPUInfo, providing the symbol that sharpyuv.c expects.
cc_library(
    name = "sharpyuv_cpu",
    srcs = ["src/dsp/cpu.c"],
    hdrs = [
        "src/dsp/cpu.h",
        "src/webp/types.h",
    ],
    includes = ["."],
    local_defines = ["VP8GetCPUInfo=SharpYuvGetCPUInfo"],
    visibility = ["//visibility:private"],
)

cc_library(
    name = "libwebp",
    srcs = glob([
        "src/dsp/*.c",
        "src/dsp/*.h",
        "src/utils/*.c",
        "src/utils/*.h",
        "src/dec/*.c",
        "src/dec/*.h",
        "src/mux/*.c",
        "src/mux/*.h",
        "src/demux/*.c",
        "src/demux/*.h",
        "src/enc/*.c",
        "src/enc/*.h",
        "sharpyuv/*.c",
        "sharpyuv/*.h",
    ], exclude = [
        "src/dsp/*_sse2.c",
        "src/dsp/*_sse41.c",
        "sharpyuv/sharpyuv_sse2.c",
        "sharpyuv/sharpyuv_cpu.c",
    ]) + [
        "imageio/imageio_util.c",
        "imageio/webpdec.c",
        "imageio/metadata.c",
    ],
    hdrs = glob([
        "src/webp/*.h",
        "examples/unicode.h",
    ]) + [
        "imageio/webpdec.h",
        "imageio/metadata.h",
        "imageio/imageio_util.h",
    ],
    copts = [],
    defines = [],
    includes = [
        ".",
        "src",
    ],
    linkopts = [],
    visibility = ["//visibility:public"],
    deps = [
        ":libwebp_sse",
        ":sharpyuv_cpu",
    ],
)
"""

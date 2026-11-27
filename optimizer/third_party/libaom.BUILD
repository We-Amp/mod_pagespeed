# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# libaom - AV1 codec library (encoder + decoder)
# Built via cmake_build rule invoking cmake directly.  The previous
# rules_foreign_cc cmake() wrapper is broken on Windows
# (pkgconfig_tool_msvc_build fails).  libaom needs cmake for
# arch-specific SIMD feature detection.
#
# Requires on PATH: cmake, a C/C++ compiler, Perl (for RTCD headers).
# On Windows: MSVC (via vcvars64), Ninja, and MSYS2 coreutils.

load("@pagespeed_2//bazel:cmake_build.bzl", "cmake_build")
load("@rules_cc//cc:defs.bzl", "cc_library")

_CMAKE_FLAGS = [
    "-DCMAKE_BUILD_TYPE=Release",
    # AVIF is an OUTPUT-only format here: read_image.cc decodes only
    # PNG/JPEG/WebP/GIF have scanline readers but AVIF does not, and the
    # SSIMULACRA2 verify floor must decode its own AVIF candidates
    # (#1274: the floor silently no-opped while the decoder was absent),
    # so the AV1 decoder is compiled in. (Pairs with AVIF_CODEC_AOM_DECODE
    # in libavif.BUILD.)
    "-DCONFIG_AV1_DECODER=1",
    "-DCONFIG_AV1_ENCODER=1",
    "-DCONFIG_MULTITHREAD=1",
    "-DENABLE_DOCS=0",
    "-DENABLE_EXAMPLES=0",
    "-DENABLE_TESTS=0",
    "-DENABLE_TOOLS=0",
    "-DBUILD_SHARED_LIBS=0",
    # The static C/C++ runtime, as everything else in the Windows build
    # (.bazelrc: static_link_msvcrt). Without it CMake defaults to /MD and
    # this archive alone makes factory_worker.exe import vcruntime140.dll,
    # which a stock Windows Server does not have. Ignored by non-MSVC
    # compilers.
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
]

cmake_build(
    name = "build_aom",
    srcs = glob(["**"]),
    out = select({
        "@platforms//os:windows": "aom.lib",
        "//conditions:default": "libaom.a",
    }),
    cmake_flags = _CMAKE_FLAGS,
    cmake_lists = "CMakeLists.txt",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "libaom",
    srcs = [":build_aom"],
    hdrs = glob(["aom/*.h"]),
    includes = ["."],
    linkopts = select({
        "@platforms//os:linux": ["-lpthread"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

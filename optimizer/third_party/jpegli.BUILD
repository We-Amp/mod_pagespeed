# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Generated export header stub for lib/cms (cmake normally generates this).
genrule(
    name = "jxl_cms_export_header",
    outs = ["jxl/jxl_cms_export.h"],
    cmd = """cat > $@ << 'GENEOF'
#ifndef JXL_CMS_EXPORT_H_
#define JXL_CMS_EXPORT_H_
#define JXL_CMS_EXPORT
#endif
GENEOF""",
)

# Jpegli - Advanced JPEG encoder from the libjxl project.
# Drop-in replacement for libjpeg-turbo with ~35% better compression
# via Butteraugli-based adaptive quantization.
#
# Source: https://github.com/google/jpegli
# License: BSD-3-Clause

# Base headers (header-only, from lib/base/)
cc_library(
    name = "base",
    hdrs = [
        "lib/base/bits.h",
        "lib/base/byte_order.h",
        "lib/base/c_callback_support.h",
        "lib/base/common.h",
        "lib/base/compiler_specific.h",
        "lib/base/data_parallel.h",
        "lib/base/fast_math-inl.h",
        "lib/base/float.h",
        "lib/base/include_jpeglib.h",
        "lib/base/matrix_ops.h",
        "lib/base/memory_manager.h",
        "lib/base/os_macros.h",
        "lib/base/override.h",
        "lib/base/parallel_runner.h",
        "lib/base/printf_macros.h",
        "lib/base/random.h",
        "lib/base/rational_polynomial-inl.h",
        "lib/base/rect.h",
        "lib/base/sanitizer_definitions.h",
        "lib/base/sanitizers.h",
        "lib/base/span.h",
        "lib/base/status.h",
        "lib/base/types.h",
    ],
    includes = ["."],
    deps = ["@highway//:hwy"],
)

# Jpegli JPEG encoder/decoder library.
# Provides jpeg_* functions that are ABI-compatible with libjpeg-turbo
# but produce substantially smaller files at equivalent visual quality.
cc_library(
    name = "jpegli",
    srcs = [
        "lib/jpegli/adaptive_quantization.cc",
        "lib/jpegli/bit_writer.cc",
        "lib/jpegli/bitstream.cc",
        "lib/jpegli/color_quantize.cc",
        "lib/jpegli/color_transform.cc",
        "lib/jpegli/common.cc",
        "lib/jpegli/decode.cc",
        "lib/jpegli/decode_marker.cc",
        "lib/jpegli/decode_scan.cc",
        "lib/jpegli/destination_manager.cc",
        "lib/jpegli/downsample.cc",
        "lib/jpegli/encode.cc",
        "lib/jpegli/encode_finish.cc",
        "lib/jpegli/encode_streaming.cc",
        "lib/jpegli/entropy_coding.cc",
        "lib/jpegli/error.cc",
        "lib/jpegli/huffman.cc",
        "lib/jpegli/idct.cc",
        "lib/jpegli/input.cc",
        "lib/jpegli/memory_manager.cc",
        "lib/jpegli/quant.cc",
        "lib/jpegli/render.cc",
        "lib/jpegli/simd.cc",
        "lib/jpegli/source_manager.cc",
        "lib/jpegli/upsample.cc",
        # ABI wrapper: maps jpeg_* names to jpegli_* implementations.
        "lib/jpegli/libjpeg_wrapper.cc",
    ],
    hdrs = [
        "lib/jpegli/adaptive_quantization.h",
        "lib/jpegli/bit_writer.h",
        "lib/jpegli/bitstream.h",
        "lib/jpegli/color_quantize.h",
        "lib/jpegli/color_transform.h",
        "lib/jpegli/common.h",
        "lib/jpegli/common_internal.h",
        "lib/jpegli/dct-inl.h",
        "lib/jpegli/decode.h",
        "lib/jpegli/decode_internal.h",
        "lib/jpegli/decode_marker.h",
        "lib/jpegli/decode_scan.h",
        "lib/jpegli/downsample.h",
        "lib/jpegli/encode.h",
        "lib/jpegli/encode_finish.h",
        "lib/jpegli/encode_internal.h",
        "lib/jpegli/encode_streaming.h",
        "lib/jpegli/entropy_coding.h",
        "lib/jpegli/entropy_coding-inl.h",
        "lib/jpegli/error.h",
        "lib/jpegli/huffman.h",
        "lib/jpegli/idct.h",
        "lib/jpegli/input.h",
        "lib/jpegli/memory_manager.h",
        "lib/jpegli/quant.h",
        "lib/jpegli/render.h",
        "lib/jpegli/simd.h",
        "lib/jpegli/transpose-inl.h",
        "lib/jpegli/types.h",
        "lib/jpegli/upsample.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":base",
        "@highway//:hwy",
        "@libjpeg_turbo",
    ],
    # libjpeg_wrapper.cc provides ABI-compatible jpeg_* symbols that interpose
    # libjpeg-turbo. Without alwayslink, the linker strips these "unreferenced"
    # interposition symbols.
    alwayslink = True,
)

# Color Management System — bridges skcms into the jxl color pipeline.
# Required by SSIMULACRA2 for perceptual quality measurement.
cc_library(
    name = "cms",
    srcs = ["lib/cms/jxl_cms.cc"],
    hdrs = [
        "lib/cms/cms.h",
        "lib/cms/cms_interface.h",
        "lib/cms/color_encoding.h",
        "lib/cms/color_encoding_cms.h",
        "lib/cms/color_encoding_internal.h",
        "lib/cms/jxl_cms_internal.h",
        "lib/cms/opsin_params.h",
        "lib/cms/tone_mapping.h",
        "lib/cms/transfer_functions.h",
    ],
    copts = ["-DJPEGXL_ENABLE_SKCMS=1"],
    includes = ["."],
    textual_hdrs = [
        "lib/cms/tone_mapping-inl.h",
        "lib/cms/transfer_functions-inl.h",
        ":jxl_cms_export_header",
    ],
    deps = [
        ":base",
        "@highway//:hwy",
        "@skcms",
    ],
)

# Image planes, PackedPixelFile, color transforms — needed by SSIMULACRA2.
cc_library(
    name = "extras",
    srcs = [
        "lib/extras/image.cc",
        "lib/extras/image_color_transform.cc",
        "lib/extras/memory_manager_internal.cc",
        "lib/extras/packed_image_convert.cc",
        "lib/extras/simd_util.cc",
        "lib/extras/xyb_transform.cc",
    ],
    hdrs = [
        "lib/extras/codestream_header.h",
        "lib/extras/image.h",
        "lib/extras/image_color_transform.h",
        "lib/extras/image_ops.h",
        "lib/extras/memory_manager_internal.h",
        "lib/extras/packed_image.h",
        "lib/extras/packed_image_convert.h",
        "lib/extras/simd_util.h",
        "lib/extras/xyb_transform.h",
    ],
    copts = ["-DJPEGXL_ENABLE_SKCMS=1"],
    includes = ["."],
    textual_hdrs = [
        "lib/extras/convolve.h",
        "lib/extras/convolve-inl.h",
    ],
    deps = [
        ":base",
        ":cms",
        "@highway//:hwy",
    ],
)

# SSIMULACRA2 — perceptual image quality metric from the JPEG XL team.
# Score range: 90+ imperceptible, 70-90 high quality, 50-70 acceptable.
cc_library(
    name = "ssimulacra2",
    srcs = [
        "tools/gauss_blur.cc",
        "tools/no_memory_manager.cc",
        "tools/ssimulacra2.cc",
    ],
    hdrs = [
        "tools/gauss_blur.h",
        "tools/no_memory_manager.h",
        "tools/ssimulacra2.h",
    ],
    copts = ["-DJPEGXL_ENABLE_SKCMS=1"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":base",
        ":cms",
        ":extras",
        "@highway//:hwy",
    ],
)

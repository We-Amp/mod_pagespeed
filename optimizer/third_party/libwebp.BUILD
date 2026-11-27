# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Upstream libwebp has platform-conditional unused functions
WEBP_COPTS = select({
    "@platforms//os:windows": [],
    "//conditions:default": ["-Wno-unused-function"],
})

# WebP decoder library - provides WebPGetFeatures for image type detection
# This is a flat library that includes all dependencies needed for decoding,
# but excludes encoding sources that require sharpyuv.
cc_library(
    name = "webp_decode",
    srcs = [
        # Decoder
        "src/dec/alpha_dec.c",
        "src/dec/buffer_dec.c",
        "src/dec/frame_dec.c",
        "src/dec/idec_dec.c",
        "src/dec/io_dec.c",
        "src/dec/quant_dec.c",
        "src/dec/tree_dec.c",
        "src/dec/vp8_dec.c",
        "src/dec/vp8l_dec.c",
        "src/dec/webp_dec.c",
        # DSP (needed by decoder)
        "src/dsp/alpha_processing.c",
        "src/dsp/cost.c",
        "src/dsp/cpu.c",
        "src/dsp/dec.c",
        "src/dsp/dec_clip_tables.c",
        "src/dsp/enc.c",
        "src/dsp/filters.c",
        "src/dsp/lossless.c",
        "src/dsp/lossless_enc.c",
        "src/dsp/rescaler.c",
        "src/dsp/ssim.c",
        "src/dsp/upsampling.c",
        "src/dsp/yuv.c",
        # NEON implementations for ARM
        "src/dsp/alpha_processing_neon.c",
        "src/dsp/cost_neon.c",
        "src/dsp/dec_neon.c",
        "src/dsp/enc_neon.c",
        "src/dsp/filters_neon.c",
        "src/dsp/lossless_enc_neon.c",
        "src/dsp/lossless_neon.c",
        "src/dsp/rescaler_neon.c",
        "src/dsp/upsampling_neon.c",
        "src/dsp/yuv_neon.c",
        # SSE implementations for x86
        "src/dsp/alpha_processing_sse2.c",
        "src/dsp/alpha_processing_sse41.c",
        "src/dsp/dec_sse2.c",
        "src/dsp/dec_sse41.c",
        "src/dsp/filters_sse2.c",
        "src/dsp/lossless_sse2.c",
        "src/dsp/lossless_sse41.c",
        "src/dsp/rescaler_sse2.c",
        "src/dsp/upsampling_sse2.c",
        "src/dsp/upsampling_sse41.c",
        "src/dsp/yuv_sse2.c",
        "src/dsp/yuv_sse41.c",
        "src/dsp/cost_sse2.c",
        "src/dsp/enc_sse2.c",
        "src/dsp/enc_sse41.c",
        "src/dsp/lossless_enc_sse2.c",
        "src/dsp/lossless_enc_sse41.c",
        "src/dsp/ssim_sse2.c",
        # AVX2 implementations for x86 (MSVC enables AVX2 by default)
        "src/dsp/lossless_avx2.c",
        "src/dsp/lossless_enc_avx2.c",
        # Utils (needed by decoder and dsp)
        "src/utils/bit_reader_utils.c",
        "src/utils/bit_writer_utils.c",
        "src/utils/color_cache_utils.c",
        "src/utils/filters_utils.c",
        "src/utils/huffman_encode_utils.c",
        "src/utils/huffman_utils.c",
        "src/utils/palette.c",
        "src/utils/quant_levels_dec_utils.c",
        "src/utils/quant_levels_utils.c",
        "src/utils/random_utils.c",
        "src/utils/rescaler_utils.c",
        "src/utils/thread_utils.c",
        "src/utils/utils.c",
    ],
    hdrs = [
        # Public API
        "src/webp/decode.h",
        "src/webp/encode.h",
        "src/webp/format_constants.h",
        "src/webp/mux_types.h",
        "src/webp/types.h",
        # Decoder internals
        "src/dec/alphai_dec.h",
        "src/dec/common_dec.h",
        "src/dec/vp8_dec.h",
        "src/dec/vp8i_dec.h",
        "src/dec/vp8li_dec.h",
        "src/dec/webpi_dec.h",
        # DSP internals
        "src/dsp/common_sse2.h",
        "src/dsp/common_sse41.h",
        "src/dsp/cpu.h",
        "src/dsp/dsp.h",
        "src/dsp/lossless.h",
        "src/dsp/lossless_common.h",
        "src/dsp/mips_macro.h",
        "src/dsp/msa_macro.h",
        "src/dsp/neon.h",
        "src/dsp/quant.h",
        "src/dsp/yuv.h",
        # Encoder headers (needed by dsp/lossless.h)
        "src/enc/backward_references_enc.h",
        "src/enc/cost_enc.h",
        "src/enc/histogram_enc.h",
        "src/enc/vp8i_enc.h",
        "src/enc/vp8li_enc.h",
        # Utils internals
        "src/utils/bit_reader_inl_utils.h",
        "src/utils/bit_reader_utils.h",
        "src/utils/bit_writer_utils.h",
        "src/utils/color_cache_utils.h",
        "src/utils/endian_inl_utils.h",
        "src/utils/filters_utils.h",
        "src/utils/huffman_encode_utils.h",
        "src/utils/huffman_utils.h",
        "src/utils/palette.h",
        "src/utils/quant_levels_dec_utils.h",
        "src/utils/quant_levels_utils.h",
        "src/utils/random_utils.h",
        "src/utils/rescaler_utils.h",
        "src/utils/thread_utils.h",
        "src/utils/utils.h",
    ],
    copts = WEBP_COPTS,
    includes = ["."],
    visibility = ["//visibility:public"],
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
    copts = WEBP_COPTS,
    includes = ["."],
    local_defines = ["VP8GetCPUInfo=SharpYuvGetCPUInfo"],
)

# SharpYUV library - needed by encoder for sharp RGB->YUV conversion
cc_library(
    name = "sharpyuv",
    srcs = [
        "sharpyuv/sharpyuv.c",
        "sharpyuv/sharpyuv_csp.c",
        "sharpyuv/sharpyuv_dsp.c",
        "sharpyuv/sharpyuv_gamma.c",
        "sharpyuv/sharpyuv_neon.c",
        "sharpyuv/sharpyuv_sse2.c",
    ],
    hdrs = [
        "sharpyuv/sharpyuv.h",
        "sharpyuv/sharpyuv_cpu.h",
        "sharpyuv/sharpyuv_csp.h",
        "sharpyuv/sharpyuv_dsp.h",
        "sharpyuv/sharpyuv_gamma.h",
    ],
    copts = WEBP_COPTS,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":sharpyuv_cpu",
        ":webp_decode",
    ],
)

# WebP encoder library
cc_library(
    name = "webp_encode",
    srcs = [
        "src/enc/alpha_enc.c",
        "src/enc/analysis_enc.c",
        "src/enc/backward_references_cost_enc.c",
        "src/enc/backward_references_enc.c",
        "src/enc/config_enc.c",
        "src/enc/cost_enc.c",
        "src/enc/filter_enc.c",
        "src/enc/frame_enc.c",
        "src/enc/histogram_enc.c",
        "src/enc/iterator_enc.c",
        "src/enc/near_lossless_enc.c",
        "src/enc/picture_csp_enc.c",
        "src/enc/picture_enc.c",
        "src/enc/picture_psnr_enc.c",
        "src/enc/picture_rescale_enc.c",
        "src/enc/picture_tools_enc.c",
        "src/enc/predictor_enc.c",
        "src/enc/quant_enc.c",
        "src/enc/syntax_enc.c",
        "src/enc/token_enc.c",
        "src/enc/tree_enc.c",
        "src/enc/vp8l_enc.c",
        "src/enc/webp_enc.c",
    ],
    copts = WEBP_COPTS,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":sharpyuv",
        ":webp_decode",
    ],
)

# WebP mux library - needed for animated WebP support
cc_library(
    name = "webp_mux",
    srcs = [
        "src/mux/anim_encode.c",
        "src/mux/muxedit.c",
        "src/mux/muxinternal.c",
        "src/mux/muxread.c",
    ] + glob(["src/mux/*.h"]),
    hdrs = [
        "src/webp/mux.h",
    ],
    copts = WEBP_COPTS,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":webp_decode",
        ":webp_encode",
    ],
)

# WebP demux library - needed for reading animated WebP
cc_library(
    name = "webp_demux",
    srcs = [
        "src/demux/anim_decode.c",
        "src/demux/demux.c",
    ],
    hdrs = [
        "src/webp/demux.h",
    ],
    copts = WEBP_COPTS,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":webp_decode"],
)

# Full libwebp with encode + decode + mux support
cc_library(
    name = "libwebp",
    visibility = ["//visibility:public"],
    deps = [
        ":webp_decode",
        ":webp_demux",
        ":webp_encode",
        ":webp_mux",
    ],
)

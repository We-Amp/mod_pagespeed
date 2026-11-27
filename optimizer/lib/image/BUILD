# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Image utility library - type detection and format utilities
cc_library(
    name = "image_util",
    srcs = ["image_util.cc"],
    hdrs = ["image_util.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/base:basictypes",
        "@libwebp//:webp_decode",
    ],
)

# Scanline status reporting
cc_library(
    name = "scanline_status",
    hdrs = ["scanline_status.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/base:printf_format",
    ],
)

# Core scanline interfaces for image reading/writing
cc_library(
    name = "scanline_interface",
    hdrs = ["scanline_interface.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_status",
        "//lib/base:basictypes",
    ],
)

# EXIF orientation handling: tag parsing, pixel baking, and the
# orientation-correcting scanline reader decorator (issue #1005)
cc_library(
    name = "exif_orientation",
    srcs = ["exif_orientation.cc"],
    hdrs = ["exif_orientation.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_interface",
        ":scanline_status",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
    ],
)

# Scanline utility functions for pixel format expansion
cc_library(
    name = "scanline_utils",
    srcs = ["scanline_utils.cc"],
    hdrs = ["scanline_utils.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        "//lib/base:message_handler",
    ],
)

# MultipleFrame API for reading/writing static and animated images
cc_library(
    name = "image_frame_interface",
    srcs = ["image_frame_interface.cc"],
    hdrs = ["image_frame_interface.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_status",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@com_google_absl//absl/strings:str_format",
    ],
)

# WebP codec - reader, writer, and animated WebP writer
cc_library(
    name = "webp",
    srcs = ["webp_optimizer.cc"],
    hdrs = ["webp_optimizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_frame_interface",
        ":image_util",
        ":scanline_interface",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@libwebp//:webp_decode",
        "@libwebp//:webp_encode",
        "@libwebp//:webp_mux",
    ],
)

# JPEG codec - reader, utilities, and optimizer
cc_library(
    name = "jpeg",
    srcs = [
        "jpeg_optimizer.cc",
        "jpeg_reader.cc",
        "jpeg_utils.cc",
    ],
    hdrs = [
        "jpeg_optimizer.h",
        "jpeg_reader.h",
        "jpeg_utils.h",
    ],
    visibility = ["//:internal"],
    deps = [
        ":exif_orientation",
        ":image_util",
        ":scanline_interface",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@jpegli",
        "@libjpeg_turbo",
    ],
)

# PNG codec - reader, writer, and optimizer
cc_library(
    name = "png",
    srcs = ["png_optimizer.cc"],
    hdrs = ["png_optimizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_interface",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@libpng",
        "@optipng//:opngreduc",
        "@zlib",
    ],
)

# GIF codec - reader for static and animated GIF images
cc_library(
    name = "gif",
    srcs = ["gif_reader.cc"],
    hdrs = ["gif_reader.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_frame_interface",
        ":image_util",
        ":png",
        ":scanline_status",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@giflib//:dgiflib",
        "@giflib//:giflib_core",
        "@libpng",
    ],
)

# Adapters between Scanline and MultipleFrame APIs
cc_library(
    name = "scanline_frame_adapter",
    srcs = ["scanline_interface_frame_adapter.cc"],
    hdrs = ["scanline_interface_frame_adapter.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_frame_interface",
        ":image_util",
        ":scanline_interface",
        ":scanline_status",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
    ],
)

# MultipleFramePaddingReader - pads frames to image dimensions
cc_library(
    name = "frame_interface_optimizer",
    srcs = ["frame_interface_optimizer.cc"],
    hdrs = ["frame_interface_optimizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_frame_interface",
        ":image_util",
        ":scanline_status",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
    ],
)

# PixelFormatOptimizer - strips unused alpha channel
cc_library(
    name = "pixel_format_optimizer",
    srcs = ["pixel_format_optimizer.cc"],
    hdrs = ["pixel_format_optimizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_interface",
        ":scanline_status",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
    ],
)

# Image resizer - area-based scanline resizer for downscaling
cc_library(
    name = "image_resizer",
    srcs = ["image_resizer.cc"],
    hdrs = ["image_resizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_util",
        ":scanline_interface",
        ":scanline_status",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
    ],
)

# Factory functions for creating image readers/writers
cc_library(
    name = "read_image",
    srcs = ["read_image.cc"],
    hdrs = ["read_image.h"],
    visibility = ["//:internal"],
    deps = [
        ":exif_orientation",
        ":frame_interface_optimizer",
        ":gif",
        ":image_frame_interface",
        ":image_util",
        ":jpeg",
        ":png",
        ":scanline_frame_adapter",
        ":scanline_interface",
        ":scanline_status",
        ":scanline_utils",
        ":webp",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
    ],
)

# Image format conversion utilities
cc_library(
    name = "image_converter",
    srcs = ["image_converter.cc"],
    hdrs = ["image_converter.h"],
    visibility = ["//:internal"],
    deps = [
        ":gif",
        ":image_frame_interface",
        ":image_util",
        ":jpeg",
        ":png",
        ":read_image",
        ":scanline_frame_adapter",
        ":scanline_interface",
        ":scanline_status",
        ":scanline_utils",
        ":webp",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@libpng",
        "@zlib",
    ],
)

# Content-aware image quality presets
cc_library(
    name = "content_analyzer",
    srcs = ["content_analyzer.cc"],
    hdrs = ["content_analyzer.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_analysis",
        ":image_util",
        "//lib/base:message_handler",
        "//lib/base:string_util",
    ],
)

# Image feature extraction for quality prediction models
cc_library(
    name = "quality_features",
    srcs = ["quality_features.cc"],
    hdrs = ["quality_features.h"],
    visibility = ["//:internal"],
    deps = [
        ":content_analyzer",
    ],
)

# Image analysis - gradient, histogram, photo detection
cc_library(
    name = "image_analysis",
    srcs = ["image_analysis.cc"],
    hdrs = ["image_analysis.h"],
    visibility = ["//:internal"],
    deps = [
        ":exif_orientation",
        ":gif",
        ":image_frame_interface",
        ":image_util",
        ":jpeg",
        ":pixel_format_optimizer",
        ":png",
        ":read_image",
        ":scanline_interface",
        ":scanline_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "@libwebp//:webp_decode",
    ],
)

# Image dimension reader - lightweight header-only dimension extraction
cc_library(
    name = "image_dimensions",
    srcs = ["image_dimensions.cc"],
    hdrs = ["image_dimensions.h"],
    visibility = ["//:internal"],
    deps = [
        ":image_analysis",
        ":image_util",
        "//lib/base:message_handler",
    ],
)

# SSIMULACRA2 perceptual quality verifier
cc_library(
    name = "quality_verifier",
    srcs = ["quality_verifier.cc"],
    hdrs = ["quality_verifier.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/base:message_handler",
        "@jpegli//:ssimulacra2",
    ],
)

# SVG sanitizer - allowlist-based structural validation
cc_library(
    name = "svg_sanitizer",
    srcs = ["svg_sanitizer.cc"],
    hdrs = ["svg_sanitizer.h"],
    visibility = ["//:internal"],
)

# SVG preprocessor - pixel pipeline for auto-vectorization
cc_library(
    name = "svg_preprocessor",
    srcs = ["svg_preprocessor.cc"],
    hdrs = ["svg_preprocessor.h"],
    visibility = ["//:internal"],
)

# VTracer FFI - C++ wrapper around the Rust VTracer staticlib.
# Provides vtracer_convert(), vtracer_free(), vtracer_default_config()
# for raster-to-SVG vectorization.
cc_library(
    name = "vtracer_ffi",
    hdrs = ["vtracer_ffi.h"],
    # Rust stdlib needs ntdll.lib on Windows (NtReadFile, NtWriteFile, etc.)
    linkopts = select({
        "@platforms//os:windows": ["-DEFAULTLIB:ntdll.lib"],
        "//conditions:default": [],
    }),
    visibility = ["//:internal"],
    deps = ["//lib/image/vtracer_ffi:vtracer_ffi_rust"],
)

# SVG rasterizer - re-rasterize SVG for fidelity verification
cc_library(
    name = "svg_rasterizer",
    srcs = ["svg_rasterizer.cc"],
    hdrs = ["svg_rasterizer.h"],
    visibility = ["//:internal"],
    deps = [
        "@nanosvg",
    ],
)

# SVG vectorizer - full raster-to-SVG pipeline
cc_library(
    name = "svg_vectorizer",
    srcs = ["svg_vectorizer.cc"],
    hdrs = ["svg_vectorizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":svg_preprocessor",
        ":svg_sanitizer",
        ":vtracer_ffi",
    ],
)

# Learned quality predictor - wraps per-format LightGBM models compiled to C
cc_library(
    name = "quality_predictor",
    srcs = ["quality_predictor.cc"],
    hdrs = ["quality_predictor.h"],
    visibility = ["//:internal"],
    deps = [
        ":quality_features",
        "//lib/image/generated:predictors",
    ],
)

# Combined image library for convenience
cc_library(
    name = "image",
    visibility = ["//:internal"],
    deps = [
        ":image_frame_interface",
        ":image_util",
        ":scanline_interface",
        ":scanline_status",
        ":scanline_utils",
    ],
)

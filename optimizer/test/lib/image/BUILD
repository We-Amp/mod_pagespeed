# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//bazel:test.bzl", "gtest_test", "image_codec_test")

# Test data files - exported for use by tests in other packages
filegroup(
    name = "testdata",
    srcs = glob(["testdata/**"]),
    visibility = ["//visibility:public"],
)

# Shared test utilities for image tests
cc_library(
    name = "test_utils",
    testonly = True,
    srcs = ["test_utils.cc"],
    hdrs = ["test_utils.h"],
    deps = [
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:image_util",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
        "//lib/image:scanline_utils",
        "@com_google_googletest//:gtest",
    ],
)

gtest_test(
    name = "image_util_test",
    deps = [
        "//lib/image:image_util",
    ],
)

gtest_test(
    name = "scanline_status_test",
    deps = [
        "//lib/image:scanline_status",
    ],
)

gtest_test(
    name = "scanline_interface_test",
    deps = [
        "//lib/image:scanline_interface",
        "//lib/image:scanline_status",
    ],
)

gtest_test(
    name = "scanline_utils_test",
    deps = [
        "//lib/base:message_handler",
        "//lib/image:image_util",
        "//lib/image:scanline_utils",
    ],
)

gtest_test(
    name = "image_frame_interface_test",
    deps = [
        "//lib/base:message_handler",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:scanline_status",
    ],
)

image_codec_test(
    name = "webp_optimizer_test",
    data_globs = [
        "testdata/*.webp",
        "testdata/*.png",
    ],
    tags = [],
    deps = [
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:scanline_status",
        "//lib/image:webp",
    ],
)

image_codec_test(
    name = "png_optimizer_test",
    data_globs = [
        "testdata/*.png",
        "testdata/png/*.png",
        "testdata/pngsuite/*.png",
    ],
    deps = [
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:png",
        "//lib/image:scanline_utils",
        "@libpng",
        "@zlib",
    ],
)

image_codec_test(
    name = "jpeg_optimizer_test",
    data_globs = ["testdata/jpeg/*"],
    deps = [
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:exif_orientation",
        "//lib/image:jpeg",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
        "//lib/image:scanline_utils",
    ],
)

image_codec_test(
    name = "exif_orientation_test",

    data_globs = ["testdata/jpeg/*"],
    deps = [
        "//lib/base:message_handler",
        "//lib/image:exif_orientation",
        "//lib/image:image_util",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
    ],
)

image_codec_test(
    name = "exif_orientation_containers_test",
    data_globs = [
        "testdata/exif/*",
        "testdata/alpha_32x32.png",
        "testdata/alpha_32x32.webp",
    ],
    deps = [
        "//lib/base:message_handler",
        "//lib/image:exif_orientation",
        "//lib/image:image_util",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
    ],
)

image_codec_test(
    name = "jpeg_utils_test",
    data_globs = ["testdata/jpeg/*"],
    deps = [
        ":test_utils",
        "//lib/base:message_handler",
        "//lib/image:jpeg",
    ],
)

image_codec_test(
    name = "jpeg_reader_test",
    data_globs = ["testdata/jpeg/*"],
    deps = [
        ":test_utils",
        "//lib/base:message_handler",
        "//lib/image:image_util",
        "//lib/image:jpeg",
    ],
)

gtest_test(
    name = "frame_interface_optimizer_test",
    deps = [
        ":test_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:frame_interface_optimizer",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:scanline_status",
    ],
)

image_codec_test(
    name = "pixel_format_optimizer_test",
    data_globs = ["testdata/webp/*.png"],
    deps = [
        ":test_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:image_util",
        "//lib/image:pixel_format_optimizer",
        "//lib/image:png",
        "//lib/image:scanline_interface",
        "//lib/image:scanline_status",
    ],
)

image_codec_test(
    name = "image_converter_test",
    data_globs = [
        "testdata/gif/*",
        "testdata/png/*.png",
        "testdata/pngsuite/*.png",
    ],
    deps = [
        ":test_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:image_converter",
        "//lib/image:image_util",
        "//lib/image:png",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
    ],
)

image_codec_test(
    name = "gif_reader_test",
    data_globs = [
        "testdata/gif/*",
        "testdata/pngsuite/gif/*",
    ],
    deps = [
        ":test_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:gif",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:png",
        "//lib/image:read_image",
        "//lib/image:scanline_frame_adapter",
        "//lib/image:scanline_utils",
        "@giflib//:giflib_core",
        "@libpng",
    ],
)

image_codec_test(
    name = "gif_to_webp_test",
    data_globs = ["testdata/gif/*"],
    deps = [
        ":test_utils",
        "//lib/base:message_handler",
        "//lib/image:image_converter",
        "//lib/image:image_util",
        "//lib/image:webp",
    ],
)

image_codec_test(
    name = "image_resizer_test",
    data_globs = [
        "testdata/pngsuite/*.png",
        "testdata/png/*.png",
        "testdata/resized/*.png",
    ],
    deps = [
        ":test_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:image_resizer",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:png",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
        "//lib/image:webp",
        "@com_google_absl//absl/strings:str_format",
    ],
)

image_codec_test(
    name = "read_image_test",
    data_globs = [
        "testdata/*.png",
        "testdata/*.webp",
        "testdata/jpeg/*",
        "testdata/gif/*",
    ],
    deps = [
        ":test_utils",
        "//lib/base:message_handler",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:png",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
        "//lib/image:scanline_status",
        "//lib/image:webp",
        "@libpng",
        "@zlib",
    ],
)

gtest_test(
    name = "image_dimensions_test",
    data = [":testdata"],
    deps = [
        "//lib/image:image_dimensions",
    ],
)

image_codec_test(
    name = "scanline_frame_adapter_test",
    data_globs = [
        "testdata/*.png",
        "testdata/*.webp",
        "testdata/gif/*",
        "testdata/jpeg/*",
    ],
    deps = [
        ":test_utils",
        "//lib/base:message_handler",
        "//lib/image:gif",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:png",
        "//lib/image:read_image",
        "//lib/image:scanline_frame_adapter",
        "//lib/image:scanline_interface",
        "//lib/image:scanline_status",
        "//lib/image:webp",
        "@libpng",
        "@zlib",
    ],
)

image_codec_test(
    name = "content_analyzer_test",
    data_globs = ["testdata/jpeg/*"],
    tags = [],
    deps = [
        ":test_utils",
        "//lib/base:message_handler",
        "//lib/image:content_analyzer",
        "//lib/image:image_analysis",
        "//lib/image:image_util",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
    ],
)

gtest_test(
    name = "quality_verifier_test",
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//lib/image:quality_verifier",
    ],
)

image_codec_test(
    name = "image_analysis_test",
    data_globs = [
        "testdata/jpeg/*",
        "testdata/pngsuite/*.png",
    ],
    deps = [
        ":test_utils",
        "//lib/base:basictypes",
        "//lib/base:message_handler",
        "//lib/image:image_analysis",
        "//lib/image:image_util",
        "//lib/image:read_image",
        "//lib/image:scanline_interface",
        "//lib/image:scanline_utils",
    ],
)

gtest_test(
    name = "quality_features_test",
    deps = [
        "//lib/image:quality_features",
    ],
)

gtest_test(
    name = "quality_predictor_test",
    deps = [
        "//lib/image:quality_features",
        "//lib/image:quality_predictor",
    ],
)

gtest_test(
    name = "svg_sanitizer_test",
    deps = [
        "//lib/image:svg_sanitizer",
    ],
)

gtest_test(
    name = "svg_preprocessor_test",
    deps = [
        "//lib/image:svg_preprocessor",
    ],
)

gtest_test(
    name = "svg_rasterizer_test",
    deps = [
        "//lib/image:svg_rasterizer",
    ],
)

gtest_test(
    name = "svg_vectorizer_test",
    deps = [
        "//lib/image:svg_vectorizer",
    ],
)

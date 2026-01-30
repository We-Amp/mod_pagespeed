load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_proto_library")

licenses(["notice"])  # Apache 2

proto_library(
    name = "image_optimizer_proto",
    srcs = ["image_optimizer.proto"],
)

cc_proto_library(
    name = "image_optimizer_proto_cc",
    deps = [":image_optimizer_proto"],
)

# =============================================================================
# Fine-grained image library targets for faster incremental builds
# =============================================================================

# Common base interfaces and utilities.
# Note: image_util.cc uses libwebp for WebP format detection in ComputeImageType().
cc_library(
    name = "image_base",
    srcs = [
        "image_frame_interface.cc",
        "image_util.cc",
        "scanline_utils.cc",
    ],
    hdrs = [
        "image_frame_interface.h",
        "image_util.h",
        "scanline_interface.h",
        "scanline_status.h",
        "scanline_utils.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "@libwebp",  # For WebP format detection in ComputeImageType()
    ],
)

# JPEG codec - depends on libjpeg-turbo.
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
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        "//pagespeed/kernel/base:pagespeed_base",
        "@libjpeg_turbo//:libjpeg",
    ],
)

# PNG codec - depends on libpng and optipng.
cc_library(
    name = "png",
    srcs = ["png_optimizer.cc"],
    hdrs = ["png_optimizer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        "//pagespeed/kernel/base:pagespeed_base",
        "@libpng",
        "@optipng//:opngreduc",
    ],
)

# WebP codec - depends on libwebp.
cc_library(
    name = "webp",
    srcs = ["webp_optimizer.cc"],
    hdrs = ["webp_optimizer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        "//pagespeed/kernel/base:pagespeed_base",
        "@libwebp",
    ],
)

# GIF codec - depends on giflib and PNG (GIF output uses PNG structures).
cc_library(
    name = "gif",
    srcs = [
        "gif_reader.cc",
        "gif_square.cc",
    ],
    hdrs = [
        "gif_reader.h",
        "gif_square.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        ":png",
        "//pagespeed/kernel/base:pagespeed_base",
        "@giflib//:dgiflib",
        "@giflib//:egiflib",
    ],
)

# Image converter - orchestrates all codecs for format conversion.
cc_library(
    name = "image_converter",
    srcs = [
        "image_converter.cc",
        "read_image.cc",
    ],
    hdrs = [
        "image_converter.h",
        "read_image.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":frame_processing",
        ":gif",
        ":image_base",
        ":jpeg",
        ":png",
        ":webp",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Frame-based image processing (animation support, optimization).
cc_library(
    name = "frame_processing",
    srcs = [
        "frame_interface_optimizer.cc",
        "pixel_format_optimizer.cc",
        "scanline_interface_frame_adapter.cc",
    ],
    hdrs = [
        "frame_interface_optimizer.h",
        "pixel_format_optimizer.h",
        "scanline_interface_frame_adapter.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Image analysis - image quality analysis and detection.
cc_library(
    name = "image_analysis",
    srcs = ["image_analysis.cc"],
    hdrs = ["image_analysis.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":frame_processing",
        ":image_base",
        ":image_converter",
        ":jpeg",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Image resizer - image scaling functionality.
cc_library(
    name = "image_resizer",
    srcs = ["image_resizer.cc"],
    hdrs = ["image_resizer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Full image optimizer - high-level optimization API.
cc_library(
    name = "image_optimizer",
    srcs = ["image_optimizer.cc"],
    hdrs = ["image_optimizer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":frame_processing",
        ":gif",
        ":image_analysis",
        ":image_base",
        ":image_converter",
        ":image_optimizer_proto_cc",
        ":image_resizer",
        ":jpeg",
        ":png",
        ":webp",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# =============================================================================
# Image codec shared libraries (for dynamic loading)
# =============================================================================
# These can be loaded at runtime using dlopen/dlsym for configurations that
# need specific image codecs without linking them all into the main binary.

cc_binary(
    name = "jpeg_codec.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":jpeg"],
)

cc_binary(
    name = "png_codec.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":png"],
)

cc_binary(
    name = "webp_codec.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":webp"],
)

cc_binary(
    name = "gif_codec.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":gif"],
)

# =============================================================================
# Backward-compatible aggregate target
# =============================================================================

# Full image library - backward compatible aggregate.
# Used by both Apache and Envoy deployments.
cc_library(
    name = "image",
    hdrs = [
        "frame_interface_optimizer.h",
        "gif_reader.h",
        "gif_square.h",
        "image_analysis.h",
        "image_converter.h",
        "image_frame_interface.h",
        "image_optimizer.h",
        "image_resizer.h",
        "image_util.h",
        "jpeg_optimizer.h",
        "jpeg_reader.h",
        "jpeg_utils.h",
        "pixel_format_optimizer.h",
        "png_optimizer.h",
        "read_image.h",
        "scanline_interface.h",
        "scanline_interface_frame_adapter.h",
        "scanline_status.h",
        "scanline_utils.h",
        "webp_optimizer.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":frame_processing",
        ":gif",
        ":image_analysis",
        ":image_base",
        ":image_converter",
        ":image_optimizer",
        ":image_optimizer_proto_cc",
        ":image_resizer",
        ":jpeg",
        ":png",
        ":webp",
    ],
)

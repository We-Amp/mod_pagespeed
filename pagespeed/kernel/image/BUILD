load("@rules_proto//proto:defs.bzl", "proto_library")
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_proto_library")
load("//bazel:libaom.bzl", "libaom_from_source")
load("//bazel:libavif.bzl", "libavif_from_source")

# load("//bazel:libdav1d.bzl", "libdav1d_from_source")  # Stream A: enable for accelerated decode

licenses(["notice"])  # Apache 2

# =============================================================================
# AVIF codec stack — built from source via
# rules_foreign_cc. libaom is the AV1 encoder+decoder; libavif wraps it and
# MUST thread it as a cmake dep (see bazel/libavif.bzl) or every encode/decode
# returns AVIF_RESULT_NO_CODEC_AVAILABLE at runtime.
#   :aom      (cmake)  AV1 reference encoder+decoder static lib
#   :avif_lib (cmake)  libavif static lib, AVIF_CODEC_AOM=SYSTEM, deps=[":aom"]
#   :dav1d    (meson)  optional faster decoder — enable via with_dav1d=True
# =============================================================================
libaom_from_source()

# libdav1d_from_source()  # Stream A: uncomment + flip with_dav1d below.

libavif_from_source()  # add with_dav1d = True once :dav1d is enabled

# AVIF codec cc_library — mirrors :webp (below). Wraps the
# libavif static lib (:avif_lib, codecs threaded via cmake) with the
# AvifFrameReader (decode + animation) and AvifFrameWriter (encode + animation).
cc_library(
    name = "avif",
    srcs = ["avif_optimizer.cc"],
    hdrs = ["avif_optimizer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":avif_lib",
        ":image_base",
        # Source include tree so the clang-tidy compdb (which never runs the
        # cmake build) can resolve "avif/avif.h" — see _LIBAVIF_SRCS_BUILD_FILE
        # in bazel/repositories.bzl.
        "@libavif_src//:source_headers",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

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

# GIF codec - DECODE only. Depends on giflib's decoder (dgiflib) and PNG
# (GIF output uses PNG structures). The product only decodes GIF; the giflib
# encoder (egiflib) is NOT linked here — it is only reachable from gif_square,
# which is test-only (see :gif_square_testonly below).
cc_library(
    name = "gif",
    srcs = [
        "gif_reader.cc",
    ],
    hdrs = [
        "gif_reader.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        ":png",
        "//pagespeed/kernel/base:pagespeed_base",
        "@giflib//:dgiflib",
    ],
)

# GIF encoder helper - TEST ONLY. gif_square writes square GIFs via giflib's
# encoder (egiflib) and is used solely by gif_reader_test to synthesize test
# fixtures. Keeping it (and egiflib) out of the shipping :gif library means the
# product never links the GIF encoder.
cc_library(
    name = "gif_square_testonly",
    testonly = True,
    srcs = ["gif_square.cc"],
    hdrs = ["gif_square.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":image_base",
        "//pagespeed/kernel/base:pagespeed_base",
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
        ":avif",
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
        ":avif",
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

# AVIF Stream B/C build-validation + encode-speed harness (NOT shipped). Encodes
# a representative image to AVIF (timed), round-trips it back through the reader,
# and compares AVIF vs WebP output size. See avif_roundtrip_main.cc.
cc_binary(
    name = "avif_roundtrip_main",
    testonly = True,
    srcs = ["avif_roundtrip_main.cc"],
    visibility = ["//visibility:public"],
    deps = [
        ":avif",
        ":image_base",
        ":image_converter",
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
        "avif_optimizer.h",
        "frame_interface_optimizer.h",
        "gif_reader.h",
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
        ":avif",
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

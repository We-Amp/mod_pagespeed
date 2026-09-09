load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

# Common test data for all image tests
TEST_DATA = glob([
    "testdata/gif/*",
    "testdata/jpeg/*",
    "testdata/png/*",
    "testdata/pngsuite/*",
    "testdata/pngsuite/gif/*",
    "testdata/resized/*",
    "testdata/webp/*",
])

pagespeed_cc_test_library(
    name = "image_test_base",
    srcs = [
        "jpeg_optimizer_test_helper.cc",
        "test_utils.cc",
    ],
    hdrs = [
        "jpeg_optimizer_test_helper.h",
        "test_utils.h",
    ],
    deps = [
        "//pagespeed/kernel/image",
        "//pagespeed/kernel/image:jpeg",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

# =============================================================================
# Fine-grained test targets for faster test iteration
# =============================================================================

# JPEG codec tests
pagespeed_cc_test(
    name = "jpeg_test",
    srcs = [
        "jpeg_optimizer_test.cc",
        "jpeg_reader_test.cc",
        "jpeg_utils_test.cc",
    ],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:jpeg",
    ],
)

# PNG codec tests
pagespeed_cc_test(
    name = "png_test",
    srcs = ["png_optimizer_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:png",
    ],
)

# WebP codec tests
pagespeed_cc_test(
    name = "webp_test",
    srcs = ["webp_optimizer_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:webp",
    ],
)

# AVIF codec tests (mirrors :webp_test wiring)
pagespeed_cc_test(
    name = "avif_test",
    srcs = ["avif_optimizer_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:avif",
    ],
)

# GIF codec tests
pagespeed_cc_test(
    name = "gif_test",
    srcs = ["gif_reader_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:gif",
        # gif_reader_test synthesizes fixtures via gif_square (giflib encoder),
        # which is test-only and no longer part of the shipping :gif library.
        "//pagespeed/kernel/image:gif_square_testonly",
    ],
)

# Image base/utility tests
pagespeed_cc_test(
    name = "image_base_test",
    srcs = [
        "image_util_test.cc",
        "scanline_status_test.cc",
    ],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:image_base",
    ],
)

# Frame processing tests
pagespeed_cc_test(
    name = "frame_test",
    srcs = [
        "frame_interface_integration_test.cc",
        "frame_interface_optimizer_test.cc",
        "pixel_format_optimizer_test.cc",
        "scanline_interface_frame_adapter_test.cc",
    ],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:frame_processing",
        "//pagespeed/kernel/image:gif",
        "//pagespeed/kernel/image:image_converter",
    ],
)

# Image converter tests
pagespeed_cc_test(
    name = "image_converter_test",
    srcs = ["image_converter_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:image_converter",
    ],
)

# Image analysis tests
pagespeed_cc_test(
    name = "image_analysis_test",
    srcs = ["image_analysis_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:image_analysis",
    ],
)

# Image resizer tests
pagespeed_cc_test(
    name = "image_resizer_test",
    srcs = ["image_resizer_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:image_resizer",
    ],
)

# Image optimizer tests
pagespeed_cc_test(
    name = "image_optimizer_test",
    srcs = ["image_optimizer_test.cc"],
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        "//pagespeed/kernel/image:image_optimizer",
    ],
)

# =============================================================================
# Backward-compatible aggregate test target
# =============================================================================

# Full image tests - backward compatible aggregate
pagespeed_cc_test(
    name = "image",
    srcs = glob(["*_test.cc"]),
    data = TEST_DATA,
    deps = [
        ":image_test_base",
        # The glob sweeps in gif_reader_test.cc, which includes gif_square.h
        # (the test-only giflib-encoder helper, no longer in the shipping :gif).
        "//pagespeed/kernel/image:gif_square_testonly",
    ],
)

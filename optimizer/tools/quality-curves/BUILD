# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary")

# Corpus validation: load images, reject animated, run ContentAnalyzer, output manifest.
cc_binary(
    name = "validate_corpus",
    srcs = ["validate_corpus.cc"],
    deps = [
        "//lib/base:message_handler",
        "//lib/base:string_util",
        "//lib/image:content_analyzer",
        "//lib/image:image_frame_interface",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:read_image",
        "//lib/image:scanline_utils",
    ],
)

# Feature extraction: decode images, run ExtractImageFeatures(), output CSV.
cc_binary(
    name = "extract_features",
    srcs = ["extract_features.cc"],
    deps = [
        "//lib/base:message_handler",
        "//lib/image:content_analyzer",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:quality_features",
        "//lib/image:read_image",
        "//lib/image:scanline_utils",
    ],
)

# Quality sweep: encode at varying quality, compute SSIMULACRA2, output CSV.
cc_binary(
    name = "quality_sweep",
    srcs = ["quality_sweep.cc"],
    deps = [
        "//lib/base:message_handler",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:quality_verifier",
        "//lib/image:read_image",
        "//lib/image:scanline_utils",
        "//lib/image:webp",
        "@libavif",
    ],
)

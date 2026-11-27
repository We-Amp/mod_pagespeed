# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# Generated predictor stubs (replaced by compiled models after training).
# Machine-generated TL2cgen decision tree code — pure arithmetic with no
# dynamic memory. Disable sanitizer instrumentation: adds no value but OOMs
# Docker builds (~4GB RSS per file under ASan). The flag is a no-op when
# sanitizers are not enabled.
cc_library(
    name = "predictors",
    srcs = [
        "avif_predictor.c",
        "jpeg_predictor.c",
        "webp_predictor.c",
    ],
    hdrs = [
        "avif_predictor.h",
        "jpeg_predictor.h",
        "webp_predictor.h",
    ],
    copts = select({
        "@platforms//os:windows": [
            "/wd4068",
            "/wd4101",
        ],
        "//conditions:default": ["-fno-sanitize=all", "-Wno-unknown-pragmas", "-Wno-unused-variable"],
    }),
    visibility = ["//:internal"],
)

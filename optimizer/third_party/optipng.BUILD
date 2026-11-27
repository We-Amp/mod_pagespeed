# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "opngreduc",
    srcs = [
        "src/opngreduc/opngreduc.c",
    ],
    hdrs = [
        "src/opngreduc/opngreduc.h",
    ],
    visibility = ["//visibility:public"],
    deps = ["@libpng"],
)

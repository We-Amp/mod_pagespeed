# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

optipng_build_rule = """
cc_library(
    name = "opngreduc",
    srcs = [
        "src/opngreduc/opngreduc.c",
    ],
    hdrs = [
        "src/opngreduc/opngreduc.h"
    ],
    # NOTE:export_dependent_settings not included.
    deps = ["@libpng//:libpng",],
    visibility = ["//visibility:public"],
)
"""
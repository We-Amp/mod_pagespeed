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
    # Our sources include these headers as "external/optipng/...". Under bzlmod
    # the repo directory carries a canonical name, so serve the headers at the
    # old spelling through _virtual_includes.
    include_prefix = "external/optipng",
    # NOTE:export_dependent_settings not included.
    deps = ["@libpng//:libpng",],
    visibility = ["//visibility:public"],
)
"""
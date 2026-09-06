# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

ed25519_build_rule = """
cc_library(
    name = "ed25519",
    srcs = [
        "src/add_scalar.c",
        "src/fe.c",
        "src/ge.c",
        "src/key_exchange.c",
        "src/keypair.c",
        "src/sc.c",
        "src/seed.c",
        "src/sha512.c",
        "src/sign.c",
        "src/verify.c",
    ],
    hdrs = [
        "src/ed25519.h",
        "src/fe.h",
        "src/fixedint.h",
        "src/ge.h",
        "src/precomp_data.h",
        "src/sc.h",
        "src/sha512.h",
    ],
    includes = ["src"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            "-Wno-unused-parameter",
            "-fno-sanitize=shift",
        ],
    }),
    visibility = ["//visibility:public"],
)
"""

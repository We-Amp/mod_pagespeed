# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# orlp/ed25519 - Portable C implementation of Ed25519
# https://github.com/orlp/ed25519
# License: zlib

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
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": [
            "-Wno-unused-parameter",
            # UB: field element arithmetic uses left shift of negative int32_t.
            # Correct on two's complement; upstream issue in orlp/ed25519.
            "-fno-sanitize=shift",
        ],
    }),
    includes = ["src"],
    visibility = ["//visibility:public"],
)

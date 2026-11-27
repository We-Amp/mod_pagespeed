# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

# llhttp - HTTP request/response parser (MIT license)
# Successor to http_parser, maintained by Node.js.
# https://github.com/nodejs/llhttp

cc_library(
    name = "llhttp",
    srcs = [
        "src/api.c",
        "src/http.c",
        "src/llhttp.c",
    ],
    hdrs = ["include/llhttp.h"],
    # Upstream llhttp parser callbacks have unused parameters
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-unused-parameter"],
    }),
    includes = ["include"],
    visibility = ["//visibility:public"],
)

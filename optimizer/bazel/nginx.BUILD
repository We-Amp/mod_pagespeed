# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build file for NGINX headers
#
# Exposes NGINX header files as a cc_library for building
# ngx_pagespeed_module.so as a dynamic nginx module.

load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # BSD-2-Clause

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "nginx",
    hdrs = [
        "objs/ngx_auto_config.h",
        "objs/ngx_auto_headers.h",
    ] + glob(
        [
            "src/core/*.h",
            "src/http/*.h",
            "src/http/modules/*.h",
            "src/http/v2/*.h",
            "src/http/v3/*.h",
            "src/event/*.h",
            "src/event/modules/*.h",
            "src/event/quic/*.h",
            "src/stream/*.h",
            "src/mail/*.h",
        ],
        allow_empty = True,
    ) + select({
        "@platforms//os:linux": glob(
            ["src/os/unix/*.h"],
            allow_empty = True,
        ),
        "@platforms//os:macos": glob(
            ["src/os/unix/*.h"],
            allow_empty = True,
        ),
        "@platforms//os:freebsd": glob(
            ["src/os/unix/*.h"],
            allow_empty = True,
        ),
        "//conditions:default": glob(
            ["src/os/unix/*.h"],
            allow_empty = True,
        ),
    }),
    copts = select({
        "@platforms//os:linux": ["-D_GNU_SOURCE"],
        "//conditions:default": [],
    }),
    includes = [
        "objs",
        "src/core",
        "src/event",
        "src/event/modules",
        "src/event/quic",
        "src/http",
        "src/http/modules",
        "src/http/v2",
        "src/http/v3",
        "src/mail",
        "src/stream",
    ] + select({
        "@platforms//os:linux": ["src/os/unix"],
        "@platforms//os:macos": ["src/os/unix"],
        "@platforms//os:freebsd": ["src/os/unix"],
        "//conditions:default": ["src/os/unix"],
    }),
    linkopts = select({
        "@platforms//os:linux": ["-lpthread"],
        "//conditions:default": [],
    }),
)

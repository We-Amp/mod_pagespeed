# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "worker_ipc",
    srcs = ["worker_ipc.cc"],
    hdrs = ["worker_ipc.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/classify:content_type",
        # For the option-context size bound only. The wire carries the payload
        # opaquely and never parses it.
        "//lib/classify:option_context",
    ],
)

cc_library(
    name = "notification_sender",
    srcs = select({
        "@platforms//os:windows": ["notification_sender_win.cc"],
        "//conditions:default": ["notification_sender.cc"],
    }),
    hdrs = ["notification_sender.h"],
    visibility = ["//visibility:public"],
    deps = [":worker_ipc"],
)

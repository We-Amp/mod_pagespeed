# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "temp_dir",
    hdrs = ["temp_dir.h"],
    visibility = [
        "//lib:__subpackages__",
        "//test:__subpackages__",
    ],
)

cc_library(
    name = "tcp_client",
    hdrs = ["tcp_client.h"],
    visibility = [
        "//lib:__subpackages__",
        "//test:__subpackages__",
    ],
    deps = ["//lib/base:string_util"],
)

cc_library(
    name = "scoped_thread_join",
    testonly = True,
    hdrs = ["scoped_thread_join.h"],
    visibility = ["//test:__subpackages__"],
)

# Event-driven wait for a running Worker to finish the notifications a test
# sent it (instead of polling a counter for a fixed number of seconds).
cc_library(
    name = "worker_quiescence",
    testonly = True,
    hdrs = ["worker_quiescence.h"],
    visibility = ["//test:__subpackages__"],
    deps = [
        ":pipe_client",
        "//src/worker:worker_lib",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "cdp_pipe",
    hdrs = ["cdp_pipe.h"],
    visibility = [
        "//lib:__subpackages__",
        "//test:__subpackages__",
    ],
    deps = ["@libuv"],
)

cc_library(
    name = "cdp_scripted_peer",
    testonly = True,
    hdrs = ["cdp_scripted_peer.h"],
    visibility = [
        "//lib:__subpackages__",
        "//test:__subpackages__",
    ],
    deps = [
        ":cdp_pipe",
        "//src/browser:cdp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

# The device-emulation setup order of a setDocumentContent render,
# checked against a fake CDP peer's received commands.
cc_library(
    name = "cdp_setup_order",
    testonly = True,
    hdrs = ["cdp_setup_order.h"],
    visibility = ["//test:__subpackages__"],
    deps = [
        "//src/browser:device_emulation",
        "@com_google_googletest//:gtest",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "png_image",
    testonly = True,
    hdrs = ["png_image.h"],
    visibility = [
        "//lib:__subpackages__",
        "//test:__subpackages__",
    ],
    deps = ["@libpng"],
)

cc_library(
    name = "pipe_client",
    hdrs = ["pipe_client.h"],
    visibility = ["//test:__subpackages__"],
)

# Test-only access to the raw substrate under PageSpeedCache.  `testonly` is
# the enforcement half of the narrowed accessor: production targets cannot
# depend on this, so there is no production path to a whole-key write.
cc_library(
    name = "cache_test_peer",
    testonly = True,
    hdrs = ["cache_test_peer.h"],
    visibility = [
        "//lib:__subpackages__",
        "//test:__subpackages__",
    ],
    deps = [
        "//lib/cache:pagespeed_cache",
        "@cyclone",
    ],
)

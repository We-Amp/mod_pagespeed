# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "notification_sender_test",
    size = "large",
    deps = [
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "notification_sender_win_test",
    # The test's server side impersonates the client (advapi32).
    linkopts = select({
        "@platforms//os:windows": ["advapi32.lib"],
        "//conditions:default": [],
    }),
    deps = [
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//src/worker:pipe_dacl",
    ],
)

gtest_test(
    name = "worker_ipc_test",
    deps = [
        "//lib/classify:option_context",
        "//src/proto:worker_ipc",
    ],
)

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "cache_flow_test",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "pipeline_test",
    size = "large",
    # The image tests wait for the worker to finish rather than for a fixed
    # time; their silence budget is 20 minutes in sanitizer builds, which the
    # default 900 s "large" timeout could not hold.
    timeout = "eternal",
    data = ["//test/lib/image:testdata"],
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//src/worker:worker_lib",
        "//test/test_util:pipe_client",
        "//test/test_util:scoped_thread_join",
        "//test/test_util:temp_dir",
        "//test/test_util:worker_quiescence",
    ],
)

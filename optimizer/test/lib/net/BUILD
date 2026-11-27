# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "ssrf_guard_test",
    deps = [
        "//lib/net:ssrf_guard",
    ],
)

gtest_test(
    name = "upstream_pin_test",
    deps = [
        "//lib/net:upstream_pin",
    ],
)

gtest_test(
    name = "fetch_policy_test",
    deps = [
        "//lib/net:fetch_policy",
    ],
)

gtest_test(
    name = "curl_fetcher_test",
    deps = [
        "//lib/net:curl_fetcher",
    ],
)

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "abi_layout_test",
    deps = [
        "//lib/pagespeed:pagespeed_api",
    ],
)

gtest_test(
    name = "abi_option_context_test",
    deps = [
        "//lib/pagespeed:pagespeed_api",
    ],
)

gtest_test(
    name = "abi_origin_state_test",
    size = "medium",
    deps = [
        "//lib/cache:cache_control_header",
        "//lib/cache:freshness",
        "//lib/classify:alternate_metadata",
        "//lib/pagespeed:pagespeed_api",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "abi_site_test",
    size = "medium",
    deps = [
        "//lib/pagespeed:pagespeed_api",
        "//src/proto:worker_ipc",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "abi_sited_html_test",
    size = "medium",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/pagespeed:pagespeed_api",
        "//test/test_util:temp_dir",
    ],
)

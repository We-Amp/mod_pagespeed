# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

# Direct lint/compile coverage for the 2.0-owned lib/js/compat/ shim headers
# (#1104 item 3): includes each compat header first-party so clang-tidy's
# header coverage no longer depends on the (lint-excluded) vendored TUs.
gtest_test(
    name = "compat_headers_test",
    deps = [
        "//lib/js:compat",
    ],
)

gtest_test(
    name = "js_tokenizer_test",
    # The 1.15 test merge took this suite from 132 to 330 cases; under TSan it
    # runs ~81s against medium's 300s limit, and CI runs 32 tests in flight, so
    # the headroom medium leaves is not enough to stay off the timeout.
    size = "large",
    data = glob(["testdata/*"]),
    deps = [
        "//lib/js:js_keywords",
        "//lib/js:js_tokenizer",
    ],
)

gtest_test(
    name = "js_minify_test",
    size = "medium",  # the 1.15 test merge grew this suite by ~30%
    data = glob(["testdata/*"]),
    deps = [
        "//lib/js:js_keywords",
        "//lib/js:js_minify",
        "//lib/js:js_tokenizer",
    ],
)

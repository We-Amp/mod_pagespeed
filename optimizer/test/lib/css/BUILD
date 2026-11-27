# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "css_minify_test",
    deps = [
        "//lib/css:css_minify",
    ],
)

gtest_test(
    name = "css_minify_corpus_test",
    srcs = [
        "css_fuzz_corpus.h",
        "css_minify_corpus_test.cc",
    ],
    deps = [
        "//lib/css:css_minify",
    ],
)

gtest_test(
    name = "css_import_flattener_test",
    deps = [
        "//lib/css:css_import_flattener",
    ],
)

gtest_test(
    name = "css_token_stream_test",
    deps = [
        "//lib/css:css_minify",
        "//lib/css:css_token_stream",
    ],
)

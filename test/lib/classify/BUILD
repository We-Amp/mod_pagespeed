# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "alternate_id_test",
    deps = [
        "//lib/classify:alternate_id",
    ],
)

gtest_test(
    name = "accept_markdown_preference_test",
    data = ["testdata/accept_markdown_vectors.txt"],
    deps = [
        "//lib/classify:capability_mask",
    ],
)

gtest_test(
    name = "capability_mask_test",
    deps = [
        "//lib/classify:alternate_id",
        "//lib/classify:capability_mask",
    ],
)

gtest_test(
    name = "alternate_metadata_test",
    deps = [
        "//lib/classify:alternate_metadata",
    ],
)

gtest_test(
    name = "alternate_metadata_v8_test",
    srcs = [
        "alternate_metadata_v8_test.cc",
        "testdata/alternate_metadata_fixtures.h",
    ],
    deps = [
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
    ],
)

gtest_test(
    name = "option_context_test",
    srcs = ["option_context_test.cc"],
    data = ["testdata/option_context_goldens.txt"],
    deps = [
        "//lib/classify:option_context",
    ],
)

gtest_test(
    name = "content_type_test",
    deps = [
        "//lib/classify:content_type",
    ],
)

gtest_test(
    name = "hostname_test",
    deps = [
        "//lib/classify:hostname",
    ],
)

gtest_test(
    name = "pagespeed_selector_test",
    deps = [
        "//lib/classify:alternate_id",
        "//lib/classify:capability_mask",
        "//lib/classify:pagespeed_selector",
        "@cyclone",
    ],
)

gtest_test(
    name = "dimension_iterator_test",
    deps = [
        "//lib/classify:dimension_iterator",
    ],
)

gtest_test(
    name = "url_normalizer_test",
    deps = [
        "//lib/classify:url_normalizer",
    ],
)

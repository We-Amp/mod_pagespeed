# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Convenience macro for GoogleTest-based cc_test targets."""

load("@rules_cc//cc:defs.bzl", "cc_test")

def gtest_test(name, deps = [], size = "small", srcs = None, **kwargs):
    """cc_test that automatically depends on gtest_main.

    Args:
        name: Test target name.
        deps: Dependencies (gtest_main is appended automatically).
        size: Test size (default: "small").
        srcs: Source files (default: [name + ".cc"]).
        **kwargs: Forwarded to cc_test (data, tags, shard_count, etc.).
    """
    if srcs == None:
        srcs = [name + ".cc"]
    cc_test(
        name = name,
        size = size,
        srcs = srcs,
        deps = deps + ["@com_google_googletest//:gtest_main"],
        **kwargs,
    )

def image_codec_test(name, data_globs, deps = [], tags = ["no_tsan"], **kwargs):
    """gtest_test for an image codec, with test data resolved via glob.

    Captures the repeated image-codec test pattern: a GoogleTest target whose
    `data` is a glob over the package's `testdata/` tree, defaulting to the
    `no_tsan` tag shared by most codec tests.

    Args:
        name: Test target name.
        data_globs: Glob patterns (relative to the package) for test data.
        deps: Dependencies (forwarded to gtest_test).
        tags: Test tags (default: ["no_tsan"]).
        **kwargs: Forwarded to gtest_test (srcs, size, etc.).
    """
    gtest_test(
        name = name,
        data = native.glob(data_globs),
        deps = deps,
        tags = tags,
        **kwargs,
    )

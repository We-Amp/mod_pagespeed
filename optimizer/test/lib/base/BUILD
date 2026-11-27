# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "arena_test",
    deps = [
        "//lib/base:arena",
    ],
)

gtest_test(
    name = "atomic_file_writer_test",
    # Symbol-interposition shims (close/unlink/fsync) confuse TSan's fd
    # tracking; skip under TSan like the codec tests.
    tags = ["no_tsan"],
    deps = [
        "//lib/base:atomic_file_writer",
        "//lib/base:posix_compat",
    ],
)

gtest_test(
    name = "string_util_test",
    deps = [
        "//lib/base",
    ],
)

gtest_test(
    name = "message_handler_test",
    deps = [
        "//lib/base:message_handler",
    ],
)

gtest_test(
    name = "json_message_handler_test",
    deps = [
        "//lib/base:json_message_handler",
    ],
)

gtest_test(
    name = "tee_message_handler_test",
    deps = [
        "//lib/base:message_handler",
        "//lib/base:tee_message_handler",
    ],
)

gtest_test(
    name = "url_util_test",
    deps = [
        "//lib/base:url_util",
    ],
)

gtest_test(
    name = "symbol_table_test",
    deps = [
        "//lib/base:atom",
        "//lib/base:symbol_table",
    ],
)

gtest_test(
    name = "writer_test",
    deps = [
        "//lib/base:inline_slist",
        "//lib/base:null_writer",
        "//lib/base:string_writer",
        "//lib/base:writer",
    ],
)

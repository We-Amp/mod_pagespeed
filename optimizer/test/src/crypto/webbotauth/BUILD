# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "verifier_test",
    deps = [
        "//src/crypto/webbotauth:webbotauth_core",
        "@com_google_absl//absl/strings",
        "@ed25519",
    ],
)

gtest_test(
    name = "static_key_directory_test",
    deps = [
        "//src/crypto/webbotauth:webbotauth_core",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "key_directory_test",
    deps = [
        "//src/crypto/webbotauth:webbotauth_core",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "rsl_cap_validator_test",
    deps = [
        "//src/crypto/webbotauth:webbotauth_core",
        "@com_google_absl//absl/strings",
        "@ed25519",
    ],
)

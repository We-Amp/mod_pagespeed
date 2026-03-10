# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# License token serialization (no crypto dependency)
cc_library(
    name = "license_token",
    srcs = ["license_token.cc"],
    hdrs = ["license_token.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@com_google_absl//absl/strings",
    ],
)

# License verifier (Ed25519 signature check)
cc_library(
    name = "license_verifier",
    srcs = ["license_verifier.cc"],
    hdrs = ["license_verifier.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":license_token",
        "@ed25519",
    ],
)

# License signer (for key generation and testing only)
cc_library(
    name = "license_signer",
    srcs = ["license_signer.cc"],
    hdrs = ["license_signer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":license_token",
        "@ed25519",
    ],
)

# License file I/O
cc_library(
    name = "license_file",
    srcs = ["license_file.cc"],
    hdrs = ["license_file.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

cc_test(
    name = "license_token_test",
    size = "small",
    srcs = ["license_token_test.cc"],
    deps = [
        ":license_token",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "license_verifier_test",
    size = "small",
    srcs = ["license_verifier_test.cc"],
    defines = ["PAGESPEED_TEST_LICENSE_KEY"],
    deps = [
        ":license_signer",
        ":license_token",
        ":license_verifier",
        "@com_google_googletest//:gtest_main",
        "@ed25519",
    ],
)

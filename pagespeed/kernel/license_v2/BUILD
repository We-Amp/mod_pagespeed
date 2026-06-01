# Copyright (c) 2024-2026 We-Amp B.V.
#
# License crypto code: canonical source is in ModPageSpeed 2.0 (src/crypto/),
# VENDORED here as checked-in source under pagespeed/kernel/license_v2/. The six files carry a GENERATED banner and are produced by
# tools/sync-crypto.sh, which applies the same #include-path rewrite the old
# genrule did from a pinned 2.0 commit. The namespace is set at compile time via
# the PAGESPEED_LICENSE_NAMESPACE=net_instaweb -D flag (the 2.0 source supports
# this macro natively), so the compiled bytes are unchanged.
#
# To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh, re-run it, and
# commit. Drift is caught by the crypto-drift CI check.

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

_LICENSE_NS = "PAGESPEED_LICENSE_NAMESPACE=net_instaweb"
_PRODUCT_ID = "PAGESPEED_PRODUCT_ID=\\\"mps1\\\""

# License token serialization (no crypto dependency)
cc_library(
    name = "license_token",
    srcs = ["license_token.cc"],
    hdrs = ["license_token.h"],
    defines = [_LICENSE_NS, _PRODUCT_ID],
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
    defines = [_LICENSE_NS, _PRODUCT_ID],
    visibility = ["//visibility:public"],
    deps = [
        ":license_token",
        "@ed25519",
    ],
)

# Test variant of license_verifier with the zero-seed Ed25519 public key.
cc_library(
    name = "license_verifier_testkey",
    testonly = True,
    srcs = ["license_verifier.cc"],
    hdrs = ["license_verifier.h"],
    defines = [_LICENSE_NS, "PAGESPEED_TEST_LICENSE_KEY", _PRODUCT_ID],
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
    defines = [_LICENSE_NS, _PRODUCT_ID],
    visibility = ["//visibility:public"],
    deps = [
        ":license_token",
        "@ed25519",
    ],
)

# Compile-time tracking metadata for license telemetry.
# Header-only: OS, arch, distribution auto-detected; server set per-port.
cc_library(
    name = "tracking_metadata",
    hdrs = ["tracking_metadata.h"],
    visibility = ["//visibility:public"],
)

# License file I/O (1.1-specific, not shared from 2.0)
cc_library(
    name = "license_file",
    srcs = ["license_file.cc"],
    hdrs = ["license_file.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

cc_binary(
    name = "generate_license_token",
    srcs = ["generate_license_token.cc"],
    deps = [
        ":license_signer",
        ":license_token",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

cc_test(
    name = "license_token_test",
    size = "small",
    srcs = ["license_token_test.cc"],
    deps = [
        ":license_token",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "license_verifier_test",
    size = "small",
    srcs = ["license_verifier_test.cc"],
    deps = [
        ":license_signer",
        ":license_token",
        ":license_verifier",
        "@googletest//:gtest_main",
        "@ed25519",
    ],
)

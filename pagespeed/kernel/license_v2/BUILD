# Copyright (c) 2024-2026 We-Amp B.V.
#
# License crypto code: canonical source is in ModPageSpeed 2.0 (src/crypto/).
# We compile from @modpagespeed2 using:
#   - local_defines to set PAGESPEED_LICENSE_NAMESPACE=net_instaweb
#     (the 2.0 source supports this macro natively)
#   - genrule to rewrite include paths (src/crypto/ -> pagespeed/kernel/license_v2/)
#
# To update: bump MODPAGESPEED2_COMMIT in bazel/repositories.bzl.
# If 2.0 adds new files to src/crypto/, update the genrule list and
# exports_files in @modpagespeed2//src/crypto:BUILD.

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

_LICENSE_NS = "PAGESPEED_LICENSE_NAMESPACE=net_instaweb"
_PRODUCT_ID = "PAGESPEED_PRODUCT_ID=\\\"mps1\\\""

# Adapt 2.0 source files: only rewrite #include paths.
# Namespace is handled at compile time via -D flag (defines, propagates to dependents).
[genrule(
    name = "adapt_" + f.replace(".", "_"),
    srcs = ["@modpagespeed2//src/crypto:" + f],
    outs = [f],
    cmd = "sed -e 's|#include \"src/crypto/|#include \"pagespeed/kernel/license_v2/|g' -e 's|#include \"lib/base/string_util.h\"|#include \"pagespeed/kernel/base/string_util.h\"|g' $< > $@",
) for f in [
    "license_token.h",
    "license_token.cc",
    "license_verifier.h",
    "license_verifier.cc",
    "license_signer.h",
    "license_signer.cc",
]]

# License token serialization (no crypto dependency)
cc_library(
    name = "license_token",
    srcs = [":adapt_license_token_cc"],
    hdrs = [":adapt_license_token_h"],
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
    srcs = [":adapt_license_verifier_cc"],
    hdrs = [":adapt_license_verifier_h"],
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
    srcs = [":adapt_license_verifier_cc"],
    hdrs = [":adapt_license_verifier_h"],
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
    srcs = [":adapt_license_signer_cc"],
    hdrs = [":adapt_license_signer_h"],
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

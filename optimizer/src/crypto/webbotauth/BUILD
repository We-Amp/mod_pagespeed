# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# AgentPass Web-Bot-Auth core, ported from mod_pagespeed 1.15. Two
# layers behind one dependency closure (@ed25519 + absl strings; the crypto
# primitive ed25519_verify comes from @ed25519 -- the only Ed25519 user left
# in the tree since the license machinery was removed):
#   * A1 observe-only RFC 9421 verifier (verifier.*, classifier.*, ...).
#   * A3 RSL-CAP capability-token enforcement (rsl_cap_token.*,
#     rsl_cap_validator.*): validates an Authorization: License token and maps
#     the verdict to an HTTP status (401/402/allow). Status only -- it never
#     settles, meters, escrows, or handles money.
cc_library(
    name = "webbotauth_core",
    srcs = [
        "base64.cc",
        "classifier.cc",
        "header_parser.cc",
        "jwks.cc",
        "key_directory.cc",
        "rsl_cap_token.cc",
        "rsl_cap_validator.cc",
        "sfv.cc",
        "signature_base.cc",
        "static_key_directory.cc",
        "verifier.cc",
    ],
    hdrs = [
        "base64.h",
        "classifier.h",
        "header_parser.h",
        "jwks.h",
        "key_directory.h",
        "rsl_cap_token.h",
        "rsl_cap_validator.h",
        "sfv.h",
        "signature_base.h",
        "static_key_directory.h",
        "verifier.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/strings",
        "@ed25519",
    ],
)

# Offline test-support CLI: mints a deterministic Ed25519 keypair and emits
# the matching JWKS + RFC 9421 Signature-Input/Signature headers (built with
# the library serializer so they are bit-identical to what the verifier
# checks). Not shipped in any runtime.
cc_binary(
    name = "webbotauth_sign_tool",
    srcs = ["webbotauth_sign_tool.cc"],
    deps = [
        ":webbotauth_core",
        "@com_google_absl//absl/strings",
        "@ed25519",
    ],
)

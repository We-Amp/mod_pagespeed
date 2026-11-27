# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "css_minify",
    srcs = ["css_minify.cc"],
    hdrs = [
        "css_minify.h",
        # Shared Phase1/Phase2 whitespace engine (single source of truth
        # for the minifier and the token-stream oracle).
        "css_phases.h",
    ],
    visibility = ["//:internal"],
)

# CSS @import flattener - inlines cached @import dependencies
cc_library(
    name = "css_import_flattener",
    srcs = ["css_import_flattener.cc"],
    hdrs = ["css_import_flattener.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/base:string_util",
        "//lib/base:url_util",
    ],
)

# The token-stream equivalence oracle (see the header): harness-local
# tooling shared by the fuzz binary and its unit test.
cc_library(
    name = "css_token_stream",
    hdrs = ["css_token_stream.h"],
    visibility = ["//:internal"],
    deps = [
        # css_token_stream.h includes css_phases.h (the shared
        # Phase1/Phase2 engine used by Normalize).
        ":css_minify",
    ],
)

# Fuzz harness for the streaming CSS minifier (css_minify.cc runs on
# untrusted CSS). Mirror of //lib/html:html_fuzz. DEFAULT-OFF via the
# `manual` tag: excluded from every `//...` wildcard, so it never builds on
# a bare `bazel build //...`.
#   bazel run -c opt --config=asan //lib/css:css_minify_fuzz -- FILE...
# For coverage-guided mode, add -DCSS_MINIFY_LIBFUZZER + -fsanitize=fuzzer
# (see the file header).
cc_binary(
    name = "css_minify_fuzz",
    srcs = ["css_minify_fuzz.cc"],
    tags = ["manual"],
    visibility = ["//:internal"],
    deps = [
        ":css_minify",
        ":css_token_stream",
    ],
)

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The kernel sources in this package (js_keywords, js_tokenizer, js_minify,
# source_map.h) are VENDORED from mod_pagespeed 1.15 at the commit pinned in
# JS_KERNEL_PIN — never edit them by hand; see CLAUDE.md.
# compat/ headers are 2.0-owned adaptation glue.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# 2.0-native stand-ins for the canonical base/util headers the vendored
# kernel includes (StringPiece/GoogleString aliases, DCHECK/LOG(DFATAL),
# RE2 glue). Header-only.
cc_library(
    name = "compat",
    hdrs = [
        "compat/basictypes.h",
        "compat/logging.h",
        "compat/re2.h",
        "compat/string.h",
        "compat/string_util.h",
    ],
    visibility = ["//:internal"],
    deps = [
        "//lib/base:string_util",
        "@com_google_re2//:re2",
    ],
)

cc_library(
    name = "js_keywords",
    srcs = ["js_keywords.cc"],
    hdrs = ["js_keywords.h"],
    visibility = ["//:internal"],
    deps = [
        ":compat",
    ],
)

cc_library(
    name = "js_tokenizer",
    srcs = ["js_tokenizer.cc"],
    hdrs = ["js_tokenizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":compat",
        ":js_keywords",
        "@com_google_re2//:re2",
    ],
)

cc_library(
    name = "js_minify",
    srcs = ["js_minify.cc"],
    hdrs = [
        "js_minify.h",
        # Vendored HEADER-ONLY: net_instaweb::source_map::Encode stays
        # declared-but-undefined (source_map.cc would drag jsoncpp; zero 2.0
        # callers — a link error on first use is the documentation).
        "source_map.h",
    ],
    visibility = ["//:internal"],
    deps = [
        ":compat",
        ":js_keywords",
        ":js_tokenizer",
    ],
)

# 2.0-owned manual mirror of the canonical 1.15 dual-oracle fuzz harness
# (mpp #688; see the file header and CLAUDE.md). tags=["manual"] like the
# css/html harnesses: built explicitly by the fuzz lane and locally, never
# part of wildcard test runs.
#   bazel run -c opt --config=asan //lib/js:js_minify_fuzz -- FILE...
# For coverage-guided mode, add -DJS_MINIFY_LIBFUZZER + -fsanitize=fuzzer
# (see the file header).
cc_binary(
    name = "js_minify_fuzz",
    srcs = ["js_minify_fuzz.cc"],
    tags = ["manual"],
    visibility = ["//:internal"],
    deps = [
        ":compat",
        ":js_minify",
        ":js_tokenizer",
    ],
)

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "cache_control_header",
    srcs = ["cache_control_header.cc"],
    hdrs = ["cache_control_header.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:alternate_metadata",
        "//lib/classify:content_type",
    ],
)

cc_library(
    name = "freshness",
    srcs = ["freshness.cc"],
    hdrs = ["freshness.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:alternate_metadata",
        "//lib/classify:content_type",
    ],
)

cc_library(
    name = "vary_storability",
    srcs = ["vary_storability.cc"],
    hdrs = ["vary_storability.h"],
    visibility = ["//:internal"],
)

cc_library(
    name = "vary_emission",
    srcs = ["vary_emission.cc"],
    hdrs = ["vary_emission.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
    ],
)

cc_library(
    name = "headers_sidecar",
    srcs = ["headers_sidecar.cc"],
    hdrs = ["headers_sidecar.h"],
    visibility = ["//:internal"],
    deps = [
        ":vary_storability",
        "//lib/classify:alternate_id",
    ],
)

cc_library(
    name = "site_purge_epoch",
    srcs = ["site_purge_epoch.cc"],
    hdrs = ["site_purge_epoch.h"],
    visibility = ["//:internal"],
)

cc_library(
    name = "origin_cache_control",
    srcs = ["origin_cache_control.cc"],
    hdrs = ["origin_cache_control.h"],
    visibility = ["//:internal"],
    deps = [
        "//lib/classify:alternate_metadata",
    ],
)

cc_library(
    name = "html_cache_policy",
    srcs = ["html_cache_policy.cc"],
    hdrs = ["html_cache_policy.h"],
    visibility = ["//:internal"],
    deps = [
        ":headers_sidecar",
        ":origin_cache_control",
        "//lib/classify:alternate_metadata",
    ],
)

cc_library(
    name = "html_cache_decision",
    srcs = ["html_cache_decision.cc"],
    hdrs = ["html_cache_decision.h"],
    visibility = ["//:internal"],
    deps = [
        ":html_cache_policy",
    ],
)

cc_library(
    name = "pagespeed_cache",
    srcs = ["cache.cc"],
    hdrs = ["cache.h"],
    visibility = ["//:internal"],
    deps = [
        ":headers_sidecar",
        ":site_purge_epoch",
        "//lib/base:atomic_file_writer",
        "//lib/base:message_handler",
        "//lib/base:posix_compat",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:hostname",
        "//lib/classify:option_context",
        "//lib/classify:pagespeed_selector",
        "@cyclone",
    ],
)

# Fuzz harness for the HTML cache policy and its decision line formatter (both
# run on untrusted headers, bodies and paths). Mirror of //lib/css:css_minify_fuzz.
# DEFAULT-OFF via the `manual` tag: excluded from every `//...` wildcard, so it
# never builds on a bare `bazel build //...`.
#   bazel run -c opt --config=asan //lib/cache:html_cache_policy_fuzz -- FILE...
# For coverage-guided mode, add -DHTML_CACHE_POLICY_LIBFUZZER + -fsanitize=fuzzer
# (see the file header).
cc_binary(
    name = "html_cache_policy_fuzz",
    srcs = [
        "html_cache_policy_fuzz.cc",
        "html_cache_policy_fuzz.h",
    ],
    tags = ["manual"],
    visibility = ["//:internal"],
    deps = [
        ":headers_sidecar",
        ":html_cache_decision",
        ":html_cache_policy",
        ":origin_cache_control",
        ":vary_storability",
        "//lib/classify:alternate_metadata",
    ],
)

# The same harness without an entry point, for the seed-replay test: the seeds
# and the oracle run in CI without libFuzzer.
cc_library(
    name = "html_cache_policy_fuzz_harness",
    testonly = True,
    srcs = ["html_cache_policy_fuzz.cc"],
    hdrs = ["html_cache_policy_fuzz.h"],
    local_defines = ["HTML_CACHE_POLICY_FUZZ_NO_MAIN"],
    visibility = ["//:internal"],
    deps = [
        ":headers_sidecar",
        ":html_cache_decision",
        ":html_cache_policy",
        ":origin_cache_control",
        ":vary_storability",
        "//lib/classify:alternate_metadata",
    ],
)

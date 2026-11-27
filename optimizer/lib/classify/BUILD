# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Shared classification library for nginx and worker.
# Contains capability masks, alternate IDs, metadata, content type
# detection, hostname normalization, and the PageSpeed selector.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "alternate_id",
    hdrs = ["alternate_id.h"],
    visibility = ["//:internal"],
)

cc_library(
    name = "capability_mask",
    srcs = ["capability_mask.cc"],
    hdrs = ["capability_mask.h"],
    visibility = ["//:internal"],
    deps = ["//lib/base:string_util"],
)

cc_library(
    name = "alternate_metadata",
    srcs = ["alternate_metadata.cc"],
    hdrs = ["alternate_metadata.h"],
    visibility = ["//:internal"],
    deps = [
        ":alternate_id",
        ":capability_mask",
        ":content_type",
    ],
)

cc_library(
    name = "option_context",
    srcs = ["option_context.cc"],
    hdrs = ["option_context.h"],
    visibility = ["//:internal"],
    # Cyclone is here only for its bundled SHA-256; nothing in this target
    # touches the cache.
    deps = ["@cyclone"],
)

cc_library(
    name = "content_type",
    srcs = ["content_type.cc"],
    hdrs = ["content_type.h"],
    visibility = ["//:internal"],
    deps = ["//lib/base:string_util"],
)

cc_library(
    name = "hostname",
    srcs = ["hostname.cc"],
    hdrs = ["hostname.h"],
    visibility = ["//:internal"],
    deps = ["//lib/base:string_util"],
)

cc_library(
    name = "pagespeed_selector",
    srcs = ["pagespeed_selector.cc"],
    hdrs = ["pagespeed_selector.h"],
    visibility = ["//:internal"],
    deps = [
        ":alternate_id",
        "@cyclone",
    ],
)

cc_library(
    name = "dimension_iterator",
    hdrs = ["dimension_iterator.h"],
    visibility = ["//:internal"],
    deps = [
        ":capability_mask",
    ],
)

cc_library(
    name = "url_normalizer",
    srcs = ["url_normalizer.cc"],
    hdrs = ["url_normalizer.h"],
    visibility = ["//:internal"],
    deps = [
        ":hostname",
        "//lib/base:string_util",
    ],
)

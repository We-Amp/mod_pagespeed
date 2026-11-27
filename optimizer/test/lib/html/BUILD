# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

# Direct lint/compile coverage for the 2.0-owned lib/html/compat/ shim
# headers (#1130): includes each compat header first-party so clang-tidy's
# header coverage no longer depends on the vendored include chains.
gtest_test(
    name = "compat_headers_test",
    deps = [
        "//lib/html:compat",
    ],
)

# Behavioral coverage for the compat MessageHandler surface (canonical 1.15
# semantics: single-format routing, Check(false) at kFatal without aborting).
gtest_test(
    name = "compat_message_handler_test",
    deps = [
        "//lib/base:writer",
        "//lib/html:compat",
    ],
)

# Behavioral coverage for the compat dialect headers (#1130): canonical
# SplitStringPieceToVector/TrimWhitespace/StringSetInsensitive semantics,
# the string_hash recurrence, the sparse_hash_map marker no-ops, and the
# minimal Timer interface.
gtest_test(
    name = "compat_string_util_test",
    deps = [
        "//lib/html:compat",
        "@com_google_absl//absl/hash",
    ],
)

# Behavioral coverage for the Tier-1 URL-validation policy seam (default
# NonEmptyUrlPolicy = byte-identical 2.0 behavior; pluggable policy).
gtest_test(
    name = "compat_google_url_test",
    deps = [
        "//lib/html:compat",
    ],
)

gtest_test(
    name = "html_name_test",
    deps = [
        "//lib/base:string_util",
        "//lib/html:html_name",
    ],
)

gtest_test(
    name = "html_keywords_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/html:compat",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_parse",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
    ],
)

gtest_test(
    name = "html_parse_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_writer",
        "//lib/base:writer",
        "//lib/html:compat",
        "//lib/html:content_type",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_node",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
    ],
)

# Inline SVG and MathML parsing by the vendored kernel (mirrors the
# canonical kernel's foreign content tests).
gtest_test(
    name = "html_foreign_content_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_writer",
        "//lib/html:compat",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_node",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
    ],
)

gtest_test(
    name = "content_type_test",
    deps = [
        "//lib/html:content_type",
    ],
)

gtest_test(
    name = "doctype_test",
    deps = [
        "//lib/html:content_type",
        "//lib/html:doctype",
    ],
)

gtest_test(
    name = "html_element_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/html:compat",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_node",
        "//lib/html:html_parse",
    ],
)

gtest_test(
    name = "subresource_collector_filter_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/html:compat",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_parse",
        "//lib/html:subresource_collector_filter",
    ],
)

gtest_test(
    name = "csp_inline_policy_test",
    deps = [
        "//lib/html:csp_inline_policy",
    ],
)

gtest_test(
    name = "markdown_extractor_filter_test",
    deps = [
        "//lib/html:html_keywords",
        "//lib/html:markdown_extractor_filter",
    ],
)

gtest_test(
    name = "safe_entity_decode_test",
    deps = [
        "//lib/html:html_keywords",
        "//lib/html:safe_entity_decode",
    ],
)

gtest_test(
    name = "sitemap_parser_test",
    deps = [
        "//lib/html:sitemap_parser",
    ],
)

gtest_test(
    name = "llms_txt_formatter_test",
    deps = [
        "//lib/html:llms_txt_formatter",
    ],
)

gtest_test(
    name = "robots_ai_directives_test",
    deps = [
        "//lib/html:robots_ai_directives",
    ],
)

gtest_test(
    name = "html_metadata_extractor_test",
    deps = [
        "//lib/html:html_keywords",
        "//lib/html:html_metadata_extractor",
    ],
)

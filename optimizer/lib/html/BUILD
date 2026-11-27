# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# HTML parsing library - curated from mod_pagespeed
# Performance-critical: uses gperf for O(1) keyword lookup

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# The vendored canonical kernel names unused parameters in a few headers
# (html_node.h's operator new/delete overloads) and .cc files
# (empty_html_filter.cc's overrides, html_writer_filter.cc) that the
# pre-vendoring 2.0 copies had unnamed. Vendored files are never edited,
# and the warnings fire in every TU that includes those headers — keep them
# visible but non-fatal across this package's targets.
HTML_PKG_COPTS = select({
    "@platforms//os:windows": [],
    "//conditions:default": ["-Wno-error=unused-parameter"],
})

# 2.0-owned stand-ins for the canonical base/kernel headers the vendored
# 1.15 HTML kernel will include (#1130): MessageHandler/NullMessageHandler/
# PrintMessageHandler and the Tier-1 URL-validation policy seam
# (GoogleUrl/UrlValidationPolicy/NonEmptyUrlPolicy) — merged in #1141/#1142 —
# plus the dialect headers the sync's include-mapping table lands on:
# string (GoogleString), string_util (StringPiece via the D1 lib/js compat
# header for single-definition safety, StrCat/StrAppend using-decls,
# SplitStringPieceToVector, TrimWhitespace, LowerString/UpperString,
# StringSetInsensitive), logging (DCHECK/CHECK families, LOG, VLOG),
# sparse_hash_map (std::unordered_map subclass), string_hash (case-folding
# hash functors), stl_util (canonical STLDeleteElements mirror), timer
# (minimal abstract Timer), html_page_limit (the page-length bound past
# which the lexer passes the rest of a page through).
# Header-only; nothing production references it yet (the vendored kernel
# lands with the first sync).
cc_library(
    name = "compat",
    hdrs = [
        "compat/google_url.h",
        "compat/html_page_limit.h",
        "compat/logging.h",
        "compat/message_handler.h",
        "compat/sparse_hash_map.h",
        "compat/string.h",
        "compat/string_hash.h",
        "compat/string_util.h",
        "compat/stl_util.h",
        "compat/timer.h",
    ],
    visibility = ["//:internal"],
    deps = [
        "//lib/base:printf_format",
        "//lib/base:string_util",
        "//lib/js:compat",
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
    ],
)

# Generate html_name.gp.cc from html_name.gperf using gperf
# Post-process to remove deprecated 'register' keyword (not allowed in C++17+)
genrule(
    name = "html_name_gperf",
    srcs = ["html_name.gperf"],
    outs = ["html_name.gp.cc"],
    cmd = "gperf -m10 $< | sed 's/register //g' > $@",
)

# HtmlName: Tag and attribute name keywords with perfect hash lookup
cc_library(
    name = "html_name",
    srcs = [":html_name_gperf"],
    hdrs = ["html_name.h"],
    # gperf-generated code uses partial initializers ({""}) that trigger
    # -Wmissing-field-initializers; suppress since we can't control gperf output.
    copts = HTML_PKG_COPTS + select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-missing-field-initializers"],
    }),
    visibility = ["//:internal"],
    deps = [
        ":compat",
    ],
)

# HtmlKeywords: HTML escape/unescape, auto-close rules, and containment rules
cc_library(
    name = "html_keywords",
    srcs = ["html_keywords.cc"],
    hdrs = ["html_keywords.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":html_name",
        "//lib/base:basictypes",
        "//lib/base:writer",
    ],
)

# HtmlFilter: Base class for HTML processing filters
cc_library(
    name = "html_filter",
    srcs = ["html_filter.cc"],
    hdrs = ["html_filter.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
    ],
)

# HtmlNode and HtmlElement: Core HTML DOM node types
# These are combined into a single library because they have circular
# header dependencies (HtmlElement extends HtmlNode, but HtmlNode needs
# to reference HtmlElement for parent pointers).
cc_library(
    name = "html_node",
    srcs = [
        "html_element.cc",
        "html_event.cc",
        "html_node.cc",
    ],
    hdrs = [
        "html_element.h",
        "html_event.h",
        "html_node.h",
    ],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":html_filter",
        ":html_keywords",
        ":html_name",
        "//lib/base:arena",
        "//lib/base:basictypes",
        "//lib/base:inline_slist",
    ],
)

# EmptyHtmlFilter: Base class for filters that don't need to override all methods
cc_library(
    name = "empty_html_filter",
    srcs = ["empty_html_filter.cc"],
    hdrs = ["empty_html_filter.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":html_filter",
    ],
)

# SubresourceCollectorFilter: Collects subresource metadata from HTML
cc_library(
    name = "subresource_collector_filter",
    srcs = ["subresource_collector_filter.cc"],
    hdrs = ["subresource_collector_filter.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":empty_html_filter",
        ":html_name",
        ":html_node",
        "//lib/base:string_util",
    ],
)

# CspInlinePolicy: shared predicates for whether a CSP would honor an inline
# <style>/<script> we inject (style-src-elem/script-src-elem fallback chains).
cc_library(
    name = "csp_inline_policy",
    srcs = ["csp_inline_policy.cc"],
    hdrs = ["csp_inline_policy.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        "//lib/base:string_util",
    ],
)

# SafeDecodeHtmlEntities: UTF-8 character-reference decode (the full HTML5
# named set) shared by the agent_optimize body-text, inline-code, and
# <meta description> extraction paths.
cc_library(
    name = "safe_entity_decode",
    srcs = ["safe_entity_decode.cc"],
    hdrs = ["safe_entity_decode.h"],
    # Generated by tools/gen-html5-entities.py.
    textual_hdrs = ["html5_named_entities.inc"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
)

# MarkdownExtractorFilter: SAX HTML -> markdown for the agent_optimize render
cc_library(
    name = "markdown_extractor_filter",
    srcs = ["markdown_extractor_filter.cc"],
    hdrs = ["markdown_extractor_filter.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":empty_html_filter",
        ":html_metadata_extractor",
        ":html_name",
        ":html_node",
        ":html_parse",
        ":safe_entity_decode",
        "//lib/base:string_util",
    ],
)

# SitemapParser: pure XML <loc> extractor for the /llms.txt builder
cc_library(
    name = "sitemap_parser",
    srcs = ["sitemap_parser.cc"],
    hdrs = ["sitemap_parser.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        "//lib/base:string_util",
    ],
)

# LlmsTxtFormatter: pure markdown assembler for the /llms.txt index
cc_library(
    name = "llms_txt_formatter",
    srcs = ["llms_txt_formatter.cc"],
    hdrs = ["llms_txt_formatter.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [],
)

# RobotsAiDirectives: pure robots.txt AI-crawler + per-page noai classifier (P6 §2.6)
cc_library(
    name = "robots_ai_directives",
    srcs = ["robots_ai_directives.cc"],
    hdrs = ["robots_ai_directives.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        "//lib/base:string_util",
    ],
)

# HtmlMetadataExtractor: SAX <title>/<meta> extractor for cheap per-page summaries (P6 D-OQ4)
cc_library(
    name = "html_metadata_extractor",
    srcs = ["html_metadata_extractor.cc"],
    hdrs = ["html_metadata_extractor.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":empty_html_filter",
        ":html_name",
        ":html_node",
        ":html_parse",
        ":safe_entity_decode",
        "//lib/base:string_util",
    ],
)

# HtmlWriterFilter: Serializes parsed HTML back to text
cc_library(
    name = "html_writer_filter",
    srcs = ["html_writer_filter.cc"],
    hdrs = ["html_writer_filter.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":html_filter",
        ":html_keywords",
        ":html_name",
        ":html_node",
        ":html_parse",
        "//lib/base:basictypes",
        "//lib/base:writer",
    ],
)

# ContentType: MIME types and content type detection
cc_library(
    name = "content_type",
    srcs = ["content_type.cc"],
    hdrs = ["content_type.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        "//lib/base:basictypes",
    ],
)

# DocType: HTML doctype declaration parsing
cc_library(
    name = "doctype",
    srcs = ["doctype.cc"],
    hdrs = ["doctype.h"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":content_type",
    ],
)

# HtmlParse and HtmlLexer: Main HTML parser and tokenizer
# These are combined into a single library because they have circular
# dependencies (HtmlLexer calls HtmlParse methods and vice versa).
cc_library(
    name = "html_parse",
    srcs = [
        "html_lexer.cc",
        "html_parse.cc",
    ],
    hdrs = [
        "html_lexer.h",
        "html_parse.h",
    ],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":content_type",
        ":doctype",
        ":html_filter",
        ":html_keywords",
        ":html_name",
        ":html_node",
        "//lib/base:arena",
        "//lib/base:atom",
        "//lib/base:basictypes",
        "//lib/base:printf_format",
        "//lib/base:symbol_table",
    ],
)


# ---------------------------------------------------------------------------
# Differential HTML parse harness (tools/html-parse-corpus/SPEC.md)
# ---------------------------------------------------------------------------

# Probe for the differential HTML parse harness: renders the
# SPEC.md v1 parse event stream for one input. Mirror of the 1.15 reference
# probe (pagespeed/kernel/html:html_parse_probe); driven by
# tools/html-parse-corpus/gen_goldens.py and check_manifest.py.
cc_binary(
    name = "html_parse_probe",
    srcs = [
        "html_parse_probe.cc",
        "html_parse_recorder.h",
    ],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":content_type",
        ":doctype",
        ":empty_html_filter",
        ":html_keywords",
        ":html_name",
        ":html_node",
        ":html_parse",
    ],
)

# Memory-safety harness for the HTML lexer/parser. Replays
# corpus files through the probe path under ASan; the sanitizer is the
# oracle. DEFAULT-OFF via the `manual` tag: excluded from every `//...`
# wildcard, so it never builds on a bare `bazel build //...`.
#   bazel run -c opt --config=asan //lib/html:html_fuzz -- tools/html-parse-corpus/seeds/*
# For coverage-guided mode, add -DHTML_PARSE_LIBFUZZER + -fsanitize=fuzzer
# (see the file header).
cc_binary(
    name = "html_fuzz",
    srcs = [
        "html_fuzz.cc",
        "html_parse_recorder.h",
    ],
    tags = ["manual"],
    visibility = ["//:internal"],
    copts = HTML_PKG_COPTS,
    deps = [
        ":compat",
        ":content_type",
        ":doctype",
        ":empty_html_filter",
        ":html_keywords",
        ":html_name",
        ":html_node",
        ":html_parse",
    ],
)

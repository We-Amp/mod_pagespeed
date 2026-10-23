load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

licenses(["notice"])  # Apache 2

# Rule-pack library: the pack model, its strict JSON loader, path/host
# matching, value templates and decision records. Portable kernel code in
# namespace pagespeed::packs; it depends only on the HTML kernel, abseil,
# RE2 and jsoncpp. RulePackFilter runs a pack over an HtmlParse; the rewriter
# runs it as the rule_packs filter (net/instaweb/rewriter/rule_pack_host.cc).

# URL equality normalization and hreflang code validation.
cc_library(
    name = "url_norm",
    srcs = ["url_norm.cc"],
    hdrs = ["url_norm.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

# Placeholder templates: parse, expand, escape.
cc_library(
    name = "template",
    srcs = ["template.cc"],
    hdrs = ["template.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

# Pack model, limits and value checks.
cc_library(
    name = "pack",
    srcs = ["pack.cc"],
    hdrs = ["pack.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":template",
        ":url_norm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@re2//:re2",
    ],
)

# Which kinds two packs of one host both carry in different modes.
cc_library(
    name = "pack_overlap",
    srcs = ["pack_overlap.cc"],
    hdrs = ["pack_overlap.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":kind_registry",
        ":pack",
        ":rule_kind",
        "@com_google_absl//absl/strings",
    ],
)

# UTF-8 validation and UTF-8-safe HTML entity decoding.
cc_library(
    name = "entity_decode",
    srcs = ["entity_decode.cc"],
    hdrs = ["entity_decode.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/html",
    ],
)

# Small helpers over jsoncpp values, shared by the loader and the kinds.
cc_library(
    name = "json_util",
    srcs = ["json_util.cc"],
    hdrs = ["json_util.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":entity_decode",
        ":pack",
        "//pagespeed/kernel/base:pagespeed_base",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@jsoncpp",
    ],
)

# The kind interface (header only).
cc_library(
    name = "rule_kind",
    hdrs = ["rule_kind.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":pack",
        "//pagespeed/kernel/base:pagespeed_base",
        "@com_google_absl//absl/status",
        "@jsoncpp",
    ],
)

# The shape checks every kind's `value` object shares.
cc_library(
    name = "rule_value_parser",
    srcs = ["rule_value_parser.cc"],
    hdrs = ["rule_value_parser.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":json_util",
        ":pack",
        ":rule_kind",
        ":template",
        ":url_norm",
        "//pagespeed/kernel/base:pagespeed_base",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@jsoncpp",
    ],
)

# One element of a kind found in a document (header only).
cc_library(
    name = "element_fact",
    hdrs = ["element_fact.h"],
    visibility = ["//visibility:public"],
)

# The page-time types the kinds, the planner and the filter share, and the
# small pure helpers the kinds use. Independent of the kind registry.
cc_library(
    name = "planner_common",
    srcs = ["planner_common.cc"],
    hdrs = ["planner_common.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":decision",
        ":element_fact",
        ":entity_decode",
        ":pack",
        ":rule_kind",
        ":template",
        "//pagespeed/kernel/html",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
    ],
)

# The rule kinds: one handler class per kind. The single-valued head kinds
# share the keep / repair / replace ladder in HeadSingletonKind.
cc_library(
    name = "kinds",
    srcs = [
        "kinds/canonical_kind.cc",
        "kinds/description_kind.cc",
        "kinds/head_singleton_kind.cc",
        "kinds/hreflang_kind.cc",
        "kinds/jsonld_kind.cc",
        "kinds/title_kind.cc",
    ],
    hdrs = [
        "kinds/canonical_kind.h",
        "kinds/description_kind.h",
        "kinds/head_singleton_kind.h",
        "kinds/hreflang_kind.h",
        "kinds/jsonld_kind.h",
        "kinds/title_kind.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":decision",
        ":element_fact",
        ":entity_decode",
        ":json_util",
        ":pack",
        ":planner_common",
        ":rule_kind",
        ":rule_value_parser",
        ":template",
        ":url_norm",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@jsoncpp",
    ],
)

# The one table of kinds (kind_table.inc) and lookup over it.
cc_library(
    name = "kind_registry",
    srcs = ["kind_registry.cc"],
    hdrs = [
        "kind_registry.h",
        "kind_table.inc",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":kinds",
        ":rule_kind",
        "//pagespeed/kernel/base:pagespeed_base",
        "@com_google_absl//absl/strings",
    ],
)

# Host, path and rule matching; table and cluster lookup.
cc_library(
    name = "matcher",
    srcs = ["matcher.cc"],
    hdrs = ["matcher.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":kind_registry",
        ":pack",
        ":url_norm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@re2//:re2",
    ],
)

# The version a pack's engine_min is compared against.
cc_library(
    name = "engine_version",
    srcs = ["engine_version.cc"],
    hdrs = ["engine_version.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb:net_instaweb_lib",
    ],
)

# JSON pack file parsing and strict validation.
cc_library(
    name = "pack_loader",
    srcs = ["pack_loader.cc"],
    hdrs = ["pack_loader.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":json_util",
        ":kind_registry",
        ":matcher",
        ":pack",
        ":rule_kind",
        ":url_norm",
        "//pagespeed/kernel/base:pagespeed_base",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@re2//:re2",
        "@jsoncpp",
    ],
)

# Decision records and the decision-log line encoder (no I/O).
cc_library(
    name = "decision",
    srcs = ["decision.cc"],
    hdrs = ["decision.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":pack",
        ":rule_kind",
        ":template",
        "@com_google_absl//absl/strings",
    ],
)

# What the HTML traversal collects about a page (header only).
cc_library(
    name = "page_facts",
    hdrs = ["page_facts.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":element_fact",
        ":kind_registry",
        ":pack",
    ],
)

# Pure planning of one decision point: facts in, per-rule plans out.
cc_library(
    name = "planner",
    srcs = ["planner.cc"],
    hdrs = ["planner.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":kind_registry",
        ":matcher",
        ":pack",
        ":page_facts",
        ":planner_common",
        ":rule_kind",
        ":template",
        ":url_norm",
        "@com_google_absl//absl/strings",
    ],
)

# What the filter needs from the server it runs in (headers, counters, the
# decision sink).
cc_library(
    name = "pack_host",
    srcs = ["pack_host.cc"],
    hdrs = ["pack_host.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":decision",
        ":template",
    ],
)

# The text formats of pack signing: the signature sidecar and the trust
# keys. No cryptography (the server verifies; pagespeed/system).
cc_library(
    name = "signature_format",
    srcs = ["signature_format.cc"],
    hdrs = ["signature_format.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

# The streaming filter that applies a pack to an HtmlParse.
cc_library(
    name = "rule_pack_filter",
    srcs = ["rule_pack_filter.cc"],
    hdrs = ["rule_pack_filter.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":decision",
        ":entity_decode",
        ":kind_registry",
        ":link_header",
        ":matcher",
        ":pack",
        ":pack_host",
        ":page_facts",
        ":planner",
        ":planner_common",
        ":template",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
    ],
)

# RFC 8288 Link header splitter and canonical link-value writer.
cc_library(
    name = "link_header",
    srcs = ["link_header.cc"],
    hdrs = ["link_header.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":decision",
        ":pack",
        ":planner_common",
        ":url_norm",
        "@com_google_absl//absl/strings",
    ],
)

# Memory-safety harness for the pack loader. Two build modes: the default
# replays the files named on the command line through LoadPack();
# -DPACK_LOADER_LIBFUZZER exposes LLVMFuzzerTestOneInput for -fsanitize=fuzzer.
# DEFAULT-OFF via the `manual` tag, like the other fuzz harnesses; the seed
# replay also runs as //test/pagespeed/kernel/packs:pack_loader_fuzz_replay_test.
#   bazel run --config=clang-asan -c opt //pagespeed/kernel/packs:pack_loader_fuzz -- \
#       tools/pack-loader-corpus/seeds/*
exports_files(["pack_loader_fuzz.cc"])

cc_binary(
    name = "pack_loader_fuzz",
    srcs = ["pack_loader_fuzz.cc"],
    tags = ["manual"],
    deps = [
        ":pack_loader",
    ],
)

# Memory-safety harness for the rule-pack filter: a fixed pack, fuzzed HTML,
# fed in fuzz-chosen chunks with a flush between them. Besides not crashing,
# it checks that report mode leaves the output byte-identical to the same
# parse without the filter. Same two build modes as the loader harness
# (-DRULE_PACK_FILTER_LIBFUZZER for the coverage-guided one); DEFAULT-OFF via
# `manual`; the seed replay also runs as
# //test/pagespeed/kernel/packs:rule_pack_filter_fuzz_replay_test.
#   bazel run --config=clang-asan -c opt //pagespeed/kernel/packs:rule_pack_filter_fuzz -- \
#       tools/rule-pack-filter-corpus/seeds/*
exports_files(["rule_pack_filter_fuzz.cc"])

cc_binary(
    name = "rule_pack_filter_fuzz",
    srcs = ["rule_pack_filter_fuzz.cc"],
    tags = ["manual"],
    deps = [
        ":pack_host",
        ":pack_loader",
        ":rule_pack_filter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
    ],
)

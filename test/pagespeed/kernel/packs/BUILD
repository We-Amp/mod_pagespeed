load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
)

licenses(["notice"])  # Apache 2

# A small mutable JSON value for building and breaking packs in tests.
cc_library(
    name = "test_json",
    testonly = True,
    hdrs = ["test_json.h"],
    deps = [
        "@googletest//:gtest",
        "@jsoncpp",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

exports_files(["testdata/edge-seo/pack.example.json"])

filegroup(
    name = "testdata",
    testonly = True,
    srcs = glob(["testdata/**"]),
    visibility = ["//test:__subpackages__"],
)

pagespeed_cc_test(
    name = "url_norm_test",
    srcs = ["url_norm_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:url_norm",
    ],
)

pagespeed_cc_test(
    name = "template_test",
    srcs = ["template_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:template",
    ],
)

pagespeed_cc_test(
    name = "signature_format_test",
    srcs = ["signature_format_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:signature_format",
    ],
)

pagespeed_cc_test(
    name = "entity_decode_test",
    srcs = ["entity_decode_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:entity_decode",
    ],
)

pagespeed_cc_test(
    name = "link_header_test",
    srcs = ["link_header_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:link_header",
    ],
)

pagespeed_cc_test(
    name = "json_util_test",
    srcs = ["json_util_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:json_util",
        "//pagespeed/kernel/packs:pack",
    ],
)

pagespeed_cc_test(
    name = "kind_registry_test",
    srcs = ["kind_registry_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:kind_registry",
        "//pagespeed/kernel/packs:page_facts",
    ],
)

pagespeed_cc_test(
    name = "decision_test",
    srcs = ["decision_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:kind_registry",
    ],
)

pagespeed_cc_test(
    name = "matcher_test",
    srcs = ["matcher_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/packs:kind_registry",
        "//pagespeed/kernel/packs:matcher",
        "//pagespeed/kernel/packs:pack",
        "//pagespeed/kernel/packs:pack_loader",
    ],
)

pagespeed_cc_test(
    name = "pack_loader_test",
    srcs = ["pack_loader_test.cc"],
    data = [":testdata"],
    shard_count = 1,
    deps = [
        ":test_json",
        "//pagespeed/kernel/packs:engine_version",
        "//pagespeed/kernel/packs:kind_registry",
        "//pagespeed/kernel/packs:matcher",
        "//pagespeed/kernel/packs:pack",
        "//pagespeed/kernel/packs:pack_loader",
        "@com_google_absl//absl/strings",
    ],
)

# The loader fuzz harness in its deterministic replay mode, over the
# checked-in seeds: the sanitizer-less floor that runs with the unit tests.
# (The ASan replay and the coverage-guided run are in the nightly fuzz
# workflow.)
cc_test(
    name = "pack_loader_fuzz_replay_test",
    srcs = ["//pagespeed/kernel/packs:pack_loader_fuzz.cc"],
    args = ["$(locations //tools/pack-loader-corpus:seeds)"],
    data = ["//tools/pack-loader-corpus:seeds"],
    linkstatic = True,
    deps = [
        "//pagespeed/kernel/packs:pack_loader",
    ],
)

# A host without response headers that records what the filter reports.
cc_library(
    name = "test_pack_host",
    testonly = True,
    hdrs = ["test_pack_host.h"],
    deps = [
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:pack_host",
        "//pagespeed/kernel/packs:template",
        "@com_google_absl//absl/strings",
    ],
)

# The kernel-level fixture runner: HtmlParse + RulePackFilter +
# HtmlWriterFilter, whole-document and chunked.
cc_library(
    name = "fixture_runner",
    testonly = True,
    srcs = ["fixture_runner.cc"],
    hdrs = ["fixture_runner.h"],
    visibility = ["//test:__subpackages__"],
    deps = [
        ":test_pack_host",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:pack",
        "//pagespeed/kernel/packs:pack_loader",
        "//pagespeed/kernel/packs:planner",
        "//pagespeed/kernel/packs:planner_common",
        "//pagespeed/kernel/packs:rule_pack_filter",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@jsoncpp",
    ],
)

pagespeed_cc_test(
    name = "fixture_runner_test",
    srcs = ["fixture_runner_test.cc"],
    data = [":testdata"],
    shard_count = 1,
    deps = [
        ":fixture_runner",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:json_util",
        "//pagespeed/kernel/packs:pack",
        "//pagespeed/kernel/packs:planner_common",
        "//pagespeed/kernel/packs:url_norm",
        "@com_google_absl//absl/strings",
        "@com_googlesource_code_re2//:re2",
    ],
)

pagespeed_cc_test(
    name = "multi_pack_fixture_test",
    srcs = ["multi_pack_fixture_test.cc"],
    data = [":testdata"],
    shard_count = 1,
    deps = [
        ":fixture_runner",
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:pack",
        "//pagespeed/kernel/packs:pack_loader",
        "@com_google_absl//absl/strings",
    ],
)

# Writes the module-specific expected files of the named cases:
#   bazel run //test/pagespeed/kernel/packs:fixture_update -- <case>...
cc_binary(
    name = "fixture_update",
    testonly = True,
    srcs = ["fixture_update.cc"],
    deps = [":fixture_runner"],
)

pagespeed_cc_test(
    name = "planner_test",
    srcs = ["planner_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:json_util",
        "//pagespeed/kernel/packs:kind_registry",
        "//pagespeed/kernel/packs:pack_loader",
        "//pagespeed/kernel/packs:page_facts",
        "//pagespeed/kernel/packs:planner",
        "//pagespeed/kernel/packs:planner_common",
        "//pagespeed/kernel/packs:url_norm",
        "@com_google_absl//absl/strings",
    ],
)

pagespeed_cc_test(
    name = "rule_pack_filter_test",
    srcs = ["rule_pack_filter_test.cc"],
    shard_count = 1,
    deps = [
        ":fixture_runner",
        ":test_pack_host",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:pack_host",
        "//pagespeed/kernel/packs:pack_loader",
        "//pagespeed/kernel/packs:planner",
        "//pagespeed/kernel/packs:planner_common",
        "//pagespeed/kernel/packs:rule_pack_filter",
        "@com_google_absl//absl/strings",
    ],
)

# The filter fuzz harness in its deterministic replay mode, over the
# checked-in seeds (the ASan replay and the coverage-guided run are in the
# nightly fuzz workflow).
cc_test(
    name = "rule_pack_filter_fuzz_replay_test",
    srcs = ["//pagespeed/kernel/packs:rule_pack_filter_fuzz.cc"],
    args = ["$(locations //tools/rule-pack-filter-corpus:seeds)"],
    data = ["//tools/rule-pack-filter-corpus:seeds"],
    linkstatic = True,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:pack_host",
        "//pagespeed/kernel/packs:pack_loader",
        "//pagespeed/kernel/packs:rule_pack_filter",
    ],
)

# Per-page cost of the filter over a 100 KB page (p50/p99, with and without
# the filter). Not a test; run it by hand:
#   bazel run -c opt //test/pagespeed/kernel/packs:rule_pack_filter_benchmark
cc_binary(
    name = "rule_pack_filter_benchmark",
    testonly = True,
    srcs = ["rule_pack_filter_benchmark.cc"],
    data = ["testdata/edge-seo/pack.example.json"],
    tags = ["manual"],
    deps = [
        ":test_pack_host",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/packs:pack_loader",
        "//pagespeed/kernel/packs:rule_pack_filter",
        "@com_google_absl//absl/strings",
    ],
)

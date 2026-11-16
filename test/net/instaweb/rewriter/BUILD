load("@rules_cc//cc:defs.bzl", "cc_test")
load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test_library(
    name = "test_base",
    srcs = [
        "critical_images_finder_test_base.cc",
        "css_rewrite_test_base.cc",
        "image_test_base.cc",
        "mock_critical_images_finder.cc",
        "mock_resource_callback.cc",
        "notifying_fetch.cc",
        "rewrite_context_test_base.cc",
        "rewrite_test_base.cc",
        "test_rewrite_driver_factory.cc",
        "test_url_namer.cc",
    ],
    hdrs = [
        "critical_images_finder_test_base.h",
        "css_rewrite_test_base.h",
        "custom_rewrite_test_base.h",
        "image_test_base.h",
        "image_testing_peer.h",
        "mock_critical_images_finder.h",
        "mock_resource_callback.h",
        "notifying_fetch.h",
        "older_result_cache.h",
        "oob_slot_index_cache.h",
        "option_context_worst_case.h",
        "prepared_style_text_recorder.h",
        "rewrite_context_test_base.h",
        "rewrite_options_test_base.h",
        "rewrite_test_base.h",
        "test_rewrite_driver_factory.h",
        "test_url_namer.h",
    ],
    deps = [
        "//net/instaweb/rewriter",
        "//test/net/instaweb/http:http_test_base",
        "//test/pagespeed/kernel/cache:cache_test_base",
        "//test/pagespeed/kernel/html:html_test_base",
        "//test/pagespeed/kernel/http:http_test_base",
        "//test/pagespeed/kernel/image:image_test_base",
        "//test/pagespeed/kernel/thread:thread_test_base",
        "//test/pagespeed/kernel/util:util_test_base",
        "//test/pagespeed/opt/http:test_base",
        "//test/pagespeed/opt/logging:test_base",
    ],
)

filegroup(
    name = "testdata",
    srcs = glob(["testdata/*"]),
    visibility = ["//visibility:public"],
)

pagespeed_cc_test(
    name = "rewriter",
    srcs = glob(
        ["*_test.cc"],
        exclude = ["rule_pack_fixture_test.cc"],
    ),
    data = [
        ":testdata",
        # The PNG lossy fallback tests (image_test, image_rewrite_filter_test).
        "//test/pagespeed/kernel/image:lossy_fallback_fixtures",
    ],
    deps = [
        "//pagespeed/kernel/packs:pack_loader",
        "//test/net/instaweb/rewriter:test_base",
    ],
)

# The rule-pack fixtures through the real driver chain (the kernel-level run
# is //test/pagespeed/kernel/packs:fixture_runner_test).
pagespeed_cc_test(
    name = "rule_pack_fixture_test",
    srcs = ["rule_pack_fixture_test.cc"],
    data = [
        "//install:rule_packs_testdata",
        "//test/pagespeed/kernel/packs:testdata",
    ],
    shard_count = 8,
    deps = [
        "//pagespeed/kernel/packs:decision",
        "//pagespeed/kernel/packs:pack",
        "//pagespeed/kernel/packs:pack_loader",
        "//pagespeed/kernel/packs:url_norm",
        "//test/net/instaweb/rewriter:test_base",
        "//test/pagespeed/kernel/packs:fixture_runner",
        "@com_google_absl//absl/strings",
    ],
)

pagespeed_cc_test(
    name = "rewriter_config",
    srcs = glob(["config/*_test.cc"]),
    deps = [
        "//test/pagespeed/automatic:test_base",
    ],
)

# Fails when a new writer of text into the page, or a new reader of CSS
# summaries, appears in the rewriter without going through the checked path
# for CSS written into <style> elements and scripts. See the test's header.
cc_test(
    name = "inline_style_writers_test",
    srcs = ["inline_style_writers_lint.cc"],
    data = ["//net/instaweb/rewriter:inline_style_writer_lint_sources"],
    env = {
        "INLINE_STYLE_LINT_SOURCES": "$(rootpaths //net/instaweb/rewriter:inline_style_writer_lint_sources)",
    },
    linkstatic = True,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//test:main",
        "//test/pagespeed/kernel/base:pagespeed_gtest",
    ],
)

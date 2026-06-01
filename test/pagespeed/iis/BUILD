load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test", "pagespeed_cc_test_library")

licenses(["notice"])  # Apache 2

# Mock IIS infrastructure for testing
# This can be built on any platform for development, but IIS-specific tests
# require Windows.
pagespeed_cc_test_library(
    name = "mock_iis",
    srcs = ["mock_iis.cc"],
    hdrs = ["mock_iis.h"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

# Test base class for IIS tests
pagespeed_cc_test_library(
    name = "iis_test_base",
    srcs = ["iis_test_base.cc"],
    hdrs = ["iis_test_base.h"],
    deps = [
        ":mock_iis",
        "//net/instaweb/rewriter:rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/thread:thread",
        "//pagespeed/kernel/util:util",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/thread:thread_test_base",
        "@com_google_googletest//:gtest",
    ],
)

# Unit tests for IIS module components
# These tests run on Windows only

pagespeed_cc_test(
    name = "license_validator_test",
    srcs = ["license_validator_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        "//pagespeed/iis:iis_module",
            ],
)

pagespeed_cc_test(
    name = "iis_config_test",
    srcs = ["iis_config_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/iis:iis_module",
            ],
)

pagespeed_cc_test(
    name = "iis_html_rewriter_test",
    srcs = ["iis_html_rewriter_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/automatic",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

pagespeed_cc_test(
    name = "iis_http_module_test",
    srcs = ["iis_http_module_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
    ],
)

pagespeed_cc_test(
    name = "iis_base_fetch_test",
    srcs = ["iis_base_fetch_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/http:http",
    ],
)

pagespeed_cc_test(
    name = "iis_inplace_resource_handler_test",
    srcs = ["iis_inplace_resource_handler_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/iis:iis_module",
            ],
)

pagespeed_cc_test(
    name = "mock_iis_test",
    srcs = ["mock_iis_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
            ],
)

pagespeed_cc_test(
    name = "iis_admin_handler_test",
    srcs = ["iis_admin_handler_test.cc"],
    target_compatible_with = ["@platforms//os:windows"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/iis:iis_module",
            ],
)

# New tests added for TEST_PARITY_PLAN.md

pagespeed_cc_test(
    name = "iis_header_util_test",
    srcs = ["iis_header_util_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/http:http",
    ],
)

pagespeed_cc_test(
    name = "iis_writer_test",
    srcs = ["iis_writer_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

pagespeed_cc_test(
    name = "iis_fetch_lifecycle_test",
    srcs = ["iis_fetch_lifecycle_test.cc"],
    deps = [
        ":iis_test_base",
        ":mock_iis",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

# Test base class for IIS tests with RewriteDriver integration
pagespeed_cc_test_library(
    name = "iis_rewrite_test_base",
    srcs = ["iis_rewrite_test_base.cc"],
    hdrs = ["iis_rewrite_test_base.h"],
    deps = [
        ":mock_iis",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
        "//test/net/instaweb/rewriter:test_base",
    ],
)

# Integration test combining IIS mock with RewriteDriver
pagespeed_cc_test(
    name = "iis_rewrite_integration_test",
    srcs = ["iis_rewrite_integration_test.cc"],
    deps = [
        ":iis_rewrite_test_base",
        ":mock_iis",
        "//net/instaweb/rewriter:rewriter",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
    ],
)

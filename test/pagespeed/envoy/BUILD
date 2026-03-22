load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test_library",
)
load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_cc_test_library",
)

licenses(["notice"])  # Apache 2

# Mock implementations for Envoy PageSpeed filter unit testing.
# Provides mocks for Envoy interfaces (StreamDecoderFilterCallbacks,
# StreamEncoderFilterCallbacks, Dispatcher) and PageSpeed interfaces
# (UrlAsyncFetcher) needed for testing http_filter.cc and
# envoy_html_rewriter.cc.
pagespeed_cc_test_library(
    name = "envoy_mocks",
    hdrs = ["mocks.h"],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//net/instaweb/http:http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
        "@envoy//source/common/buffer:buffer_lib",
        "@envoy//source/common/http:header_map_lib",
        "@envoy//test/mocks/event:event_mocks",
        "@envoy//test/mocks/http:http_mocks",
        "@envoy//test/mocks/stream_info:stream_info_mocks",
        "@googletest//:gtest",
    ],
)

# Common copts for PageSpeed Envoy tests to suppress warnings from existing code.
_PAGESPEED_TEST_COPTS = [
    "-Wno-unused-parameter",
    "-Wno-old-style-cast",
]

# Envoy tests are Linux-only (Envoy deps don't build on Windows).
# Tagged "manual" to exclude from //test/pagespeed/... wildcards on Windows.
envoy_cc_test(
    name = "envoy_vhost_config_test",
    srcs = ["envoy_vhost_config_test.cc"],
    copts = _PAGESPEED_TEST_COPTS,
    repository = "@envoy",
    tags = ["manual"],
    deps = [
        "//pagespeed/envoy:http_filter_lib",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

envoy_cc_test(
    name = "envoy_html_rewriter_test",
    srcs = ["envoy_html_rewriter_test.cc"],
    copts = _PAGESPEED_TEST_COPTS,
    repository = "@envoy",
    tags = ["manual"],
    deps = [
        "//pagespeed/envoy:http_filter_lib",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

envoy_cc_test(
    name = "envoy_server_context_test",
    srcs = ["envoy_server_context_test.cc"],
    copts = _PAGESPEED_TEST_COPTS,
    repository = "@envoy",
    tags = ["manual"],
    deps = [
        "//pagespeed/envoy:http_filter_lib",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

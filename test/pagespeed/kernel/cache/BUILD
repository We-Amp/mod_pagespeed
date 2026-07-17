load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test_library(
    name = "cache_test_base",
    srcs = glob(
        ["*.cc"],
        exclude = glob(["*_test.cc"]),
    ),
    hdrs = glob(["*.h"]),
    deps = [
        "//pagespeed/kernel/cache",
        "//test/pagespeed/kernel/thread:thread_test_base",
        "//test/pagespeed/kernel/base:pagespeed_gtest",
    ],
)

pagespeed_cc_test(
    name = "cache",
    srcs = glob(["*_test.cc"]),
    deps = [
        "//pagespeed/kernel/cache:cyclone_cache",
        # Exports the mprotect-based MappedBackendCache test harness used by
        # the zero-copy (Cyclone borrow) promotion / marker-strip tests.
        "//test/net/instaweb/http:http_test_base",
        "//test/pagespeed/kernel/cache:cache_test_base",
    ],
)

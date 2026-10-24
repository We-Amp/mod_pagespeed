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
    srcs = glob(
        ["*_test.cc"],
        exclude = ["cyclone_cache_test.cc"],
    ),
    deps = [
        # MappedBackendCache, used by the fallback and write-through tests.
        "//test/net/instaweb/http:http_test_base",
        "//test/pagespeed/kernel/cache:cache_test_base",
    ],
)

# The Cyclone tests create sparse volumes (up to 320 MiB declared) and fork
# clusters of worker processes against them; under CI disk contention their
# runtime is set by per-stripe fsync/msync flushes, not by test count (#321).
# Their own "large" target gives them a 900 s budget that neither they nor
# the other kernel cache tests deplete for each other.
pagespeed_cc_test(
    name = "cyclone_cache",
    srcs = ["cyclone_cache_test.cc"],
    size = "large",
    deps = [
        "//pagespeed/kernel/cache:cyclone_cache",
        "//test/pagespeed/kernel/cache:cache_test_base",
    ],
)

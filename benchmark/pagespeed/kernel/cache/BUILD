load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_benchmark",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_benchmark(
    name = "cache",
    srcs = ["disk_cache_speed_test.cc"],
    deps = [
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/cache:cyclone_cache",
    ],
)

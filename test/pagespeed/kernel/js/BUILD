load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test(
    name = "js",
    srcs = glob(["*_test.cc"]),
    data = glob(["testdata/third_party/*"]),
    # The JS corpus contains ??= and ??/ (nullish coalescing, the latter
    # before a regex). Those are C++ trigraph spellings; C++20 ignores
    # trigraphs but warns on each, and here they are deliberate input.
    copts = ["-Wno-trigraphs"],
    deps = [
        "//pagespeed/kernel/js",
    ],
)

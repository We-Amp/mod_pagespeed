load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test(
    name = "js",
    srcs = glob(["*_test.cc"]),
    data = glob(["testdata/third_party/*"]),
    # The JS corpus contains ?? and ??= sequences (nullish
    # coalescing) that the compiler also reads as C++ trigraphs; they
    # are deliberate test input, and each would draw a warning.
    copts = ["-Wno-trigraphs"],
    deps = [
        "//pagespeed/kernel/js",
    ],
)

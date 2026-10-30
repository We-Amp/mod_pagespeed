load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test(
    name = "js",
    srcs = glob(["*_test.cc"]),
    data = glob(["testdata/third_party/*"]),
    # JS corpus data contains ?? sequences (nullish coalescing `??`,
    # `??=`) that are deliberate test input, not C++ trigraphs.
    copts = ["-Wno-trigraphs"],
    deps = [
        "//pagespeed/kernel/js",
    ],
)

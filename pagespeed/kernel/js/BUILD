load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

cc_library(
    name = "js",
    srcs = [
        "js_keywords.cc",
        "js_minify.cc",
        "js_tokenizer.cc",
    ],
    hdrs = [
        "js_keywords.h",
        "js_minify.h",
        "js_tokenizer.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/util:util_re2",
    ],
)

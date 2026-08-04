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

# Dual-oracle fuzz harness for the JS minifier (idempotence +
# token-channel equivalence; see the file header).  Two build modes:
# default deterministic replay + -DJS_MINIFY_LIBFUZZER for
# -fsanitize=fuzzer.
cc_binary(
    name = "js_minify_fuzz",
    srcs = ["js_minify_fuzz.cc"],
    tags = ["manual"],
    deps = [":js"],
)

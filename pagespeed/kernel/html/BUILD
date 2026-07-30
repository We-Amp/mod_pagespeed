load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

genrule(
    name = "html_name_gperf",
    srcs = ["html_name.gperf"],
    outs = ["html_name.gp.cc"],
    cmd = ("gperf -m10 $< > $@"),
)

cc_library(
    name = "html",
    srcs = glob(
        ["*.cc"],
        exclude = [
            # Probe/fuzz binaries below carry their own main(); keep them
            # out of the library glob.
            "html_parse_probe.cc",
            "html_fuzz.cc",
        ],
    ) + [":html_name_gperf"],
    hdrs = glob(["*.h"]),
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
    ],
)

# Reference probe for the design record D2 differential HTML parse harness:
# renders the SPEC.md parse event stream for one input. Driven by
# tools/html-parse-corpus/gen_goldens.py; see tools/html-parse-corpus/.
cc_binary(
    name = "html_parse_probe",
    srcs = ["html_parse_probe.cc"],
    deps = [
        ":html",
    ],
)

# Memory-safety harness for the HTML lexer/parser. Replays
# corpus files through the probe path under ASan; the sanitizer is the
# oracle. DEFAULT-OFF via the `manual` tag: excluded from every `//...`
# wildcard, so it never builds on a bare `bazel build //...`.
#   bazel run --config=macos-asan -c opt //pagespeed/kernel/html:html_fuzz -- tools/html-parse-corpus/seeds/*
# For coverage-guided mode, add -DHTML_PARSE_LIBFUZZER + -fsanitize=fuzzer
# (see the file header and tools/html-parse-corpus/README.md).
cc_binary(
    name = "html_fuzz",
    srcs = ["html_fuzz.cc"],
    tags = ["manual"],
    deps = [
        ":html",
    ],
)


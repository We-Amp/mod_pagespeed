load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

cc_library(
    name = "log_shim",
    srcs = ["logging.cc"],
    hdrs = ["logging.h"],
    # spdlog's headers instantiate a deprecated fmt conversion when this TU
    # compiles; the diagnostic fires in our compile (external header, first-
    # party TU), so the external warning boundary cannot cover it. Scoped to
    # this target and flag until the spdlog/fmt bump retires it.
    copts = select({
        "@platforms//os:macos": ["-Wno-deprecated-declarations"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        "@spdlog//:spdlog",
    ],
)

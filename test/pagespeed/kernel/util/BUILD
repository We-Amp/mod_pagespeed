load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2


pagespeed_cc_test_library(
    name = "util_test_base",
    srcs = [
        "lock_manager_spammer.cc",
        "mock_nonce_generator.cc",
        "nonce_generator_test_base.cc",
    ],
    hdrs = [
        "lock_manager_spammer.h",
        "mock_nonce_generator.h",
        "nonce_generator_test_base.h",
    ],
    deps = [
        "//pagespeed/kernel/util",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/thread:thread_test_base",
        # Transitively pulled by gtest.h on newer googletest/gRPC.
        # Windows strict-deps requires direct declaration even though
        # these headers are not used explicitly by the test sources.
        # Both abseil aliases must be declared because strict-deps
        # matches on external/{repo}/ path, not header content — the
        # include search path resolves to external/abseil-cpp/ on
        # Windows builds (see comment in bazel/repositories.bzl on the
        # dup-alias workaround).
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:reflection",
        "@com_google_absl//absl/types:any",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:reflection",
        "@abseil-cpp//absl/types:any",
    ],
)

pagespeed_cc_test(
    name = "util",
    srcs = glob(["*_test.cc"]),
    deps = [
        "//test/pagespeed/kernel/util:util_test_base",
    ],
)

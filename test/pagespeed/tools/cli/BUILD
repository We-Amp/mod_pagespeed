load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test")

licenses(["notice"])  # Apache 2

# Tests of `pagespeed lint` (//pagespeed/tools/cli).

OPT_ONLY = select({
    "//pagespeed/tools/cli:opt": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

# The masking allowlist is generated from the console's sources: it must be
# non-empty and hold every entry of safe-option-names.ts (minus the ones
# matching SECRET_NAME), so a format change fails here, not silently.
pagespeed_cc_test(
    name = "value_mask_test",
    srcs = ["value_mask_test.cc"],
    data = ["//pagespeed/system:console/src/lib/data/safe-option-names.ts"],
    shard_count = 1,
    deps = ["//pagespeed/tools/cli:lint"],
)

pagespeed_cc_test(
    name = "apache_reader_test",
    srcs = ["apache_reader_test.cc"],
    shard_count = 1,
    deps = ["//pagespeed/tools/cli:lint"],
)

# Goldens: the JSON for each case under testdata/, byte for byte, on every
# platform.  The inputs reach no LOG(DFATAL) path, so this runs in every
# compilation mode.
pagespeed_cc_test(
    name = "lint_golden_test",
    srcs = [
        "lint_golden_test.cc",
        "lint_test_util.h",
    ],
    data = glob(["testdata/**"]),
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/util",
        "//pagespeed/tools/cli:lint",
    ],
)

# Every rule against every option whose value is masked, with values that
# make the option parser fail (and log): release mode only, like the binary.
pagespeed_cc_test(
    name = "lint_secret_test",
    srcs = [
        "lint_secret_test.cc",
        "lint_test_util.h",
    ],
    shard_count = 1,
    target_compatible_with = OPT_ONLY,
    deps = [
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/util",
        "//pagespeed/tools/cli:lint",
    ],
)

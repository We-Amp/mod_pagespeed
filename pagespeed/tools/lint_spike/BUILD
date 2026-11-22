load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

licenses(["notice"])  # Apache 2

# Spike: offline Apache config lint prototype.  Throwaway code.

cc_binary(
    name = "gen_safe_option_names",
    srcs = ["gen_safe_option_names.cc"],
)

genrule(
    name = "safe_option_names_inc",
    srcs = [
        "//pagespeed/system:console/src/lib/data/safe-option-names.ts",
        "//pagespeed/system:console/src/lib/utils/diagnostics.ts",
    ],
    outs = ["safe_option_names.inc"],
    cmd = "$(location :gen_safe_option_names) $(location //pagespeed/system:console/src/lib/data/safe-option-names.ts) $(location //pagespeed/system:console/src/lib/utils/diagnostics.ts) $@",
    tools = [":gen_safe_option_names"],
)

cc_library(
    name = "apache_scope",
    srcs = ["apache_scope.cc"],
    hdrs = ["apache_scope.h"],
    deps = [
        "//pagespeed/apache:apache_config_offline",
        "//pagespeed/apache:apache_directive_table",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

cc_binary(
    name = "pagespeed_lint_spike",
    srcs = [
        "lint_main.cc",
        ":safe_option_names_inc",
    ],
    deps = [
        ":apache_scope",
        # rewriter_core's rewrite_options.cc needs rewrite_query.cc and
        # javascript_library_identification.cc, which live in :rewriter.
        "//net/instaweb/rewriter",
        "//pagespeed/apache:apache_config_offline",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/util",
        "@boringssl//:crypto",
        "@spdlog//:spdlog",
        "//base:log_shim",
    ],
)

# Spike probe for the held explain: static_rewriter with a fixed clock.
cc_binary(
    name = "timer_probe",
    testonly = True,
    srcs = ["timer_probe_main.cc"],
    deps = [
        "//pagespeed/automatic:static_rewriter_lib",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/thread:thread_test_base",
    ],
)

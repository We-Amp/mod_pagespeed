load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

licenses(["notice"])  # Apache 2

package(default_visibility = ["//visibility:public"])

# `pagespeed`: offline tools for the module.  `pagespeed lint` checks an
# Apache configuration with the module's real option parser and directive
# table (//pagespeed/apache:apache_config), and links no network code.

# The binary is built in release mode only: some option parser paths that a
# bad configuration value can reach are LOG(DFATAL), which aborts a debug
# build and only logs in a release build, as the module itself ships.
config_setting(
    name = "opt",
    values = {"compilation_mode": "opt"},
)

OPT_ONLY = select({
    ":opt": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

# The admin console's list of options whose values may be shown, and its
# SECRET_NAME pattern, turned into a C++ include at build time.  There is no
# second list: edit safe-option-names.ts.
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
    cmd = "$(location :gen_safe_option_names) " +
          "$(location //pagespeed/system:console/src/lib/data/safe-option-names.ts) " +
          "$(location //pagespeed/system:console/src/lib/utils/diagnostics.ts) $@",
    tools = [":gen_safe_option_names"],
)

cc_library(
    name = "lint",
    srcs = [
        "apache_lint.cc",
        "apache_reader.cc",
        "lint_report.cc",
        "log_capture.cc",
        "value_mask.cc",
        ":safe_option_names_inc",
    ],
    hdrs = [
        "apache_lint.h",
        "apache_reader.h",
        "lint_report.h",
        "log_capture.h",
        "value_mask.h",
    ],
    deps = [
        "//base:log_shim",
        # rewrite_options.cc (in :rewriter_core) needs rewrite_query.cc and
        # javascript_library_identification.cc, which are in :rewriter.
        "//net/instaweb/rewriter",
        "//pagespeed/apache:apache_config",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/system:system_rewrite_options",
        "@boringssl//:crypto",
        "@spdlog//:spdlog",
    ],
)

cc_binary(
    name = "pagespeed",
    srcs = ["pagespeed_main.cc"],
    target_compatible_with = OPT_ONLY,
    deps = [
        ":lint",
        "//net/instaweb:net_instaweb_lib",
        "//pagespeed/kernel/html",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/util",
    ],
)

exports_files(["pagespeed-lint.schema.json"])

load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

pagespeed_cc_test_library(
    name = "test_base",
    srcs = ["mock_apache.cc"],
    hdrs = ["mock_apache.h"],
    copts = [
        "-I/usr/include/apache2/",
        "-Iexternal/aprutil/include/",
        "-Iexternal/aprutil/include/arch/unix/",
        "-Iexternal/aprutil/",
    ] + select({
        "//bazel:linux_arm64": [
            "-Ithird_party/aprutil/gen/arch/linux/arm64/include/",
            "-Ithird_party/apr/gen/arch/linux/arm64/include/",
        ],
        "//conditions:default": [
            "-Ithird_party/aprutil/gen/arch/linux/x64/include/",
            "-Ithird_party/apr/gen/arch/linux/x64/include/",
        ],
    }) + [
        "-Iexternal/apr/include/",
        "-Iexternal/apr/include/arch/unix/",
        "-Iexternal/apr/",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        # Use apache_core instead of apache to avoid the static
        # ApacheProcessContext in mod_instaweb.cc which conflicts with
        # the test infrastructure's RewriteTestBaseProcessContext.
        "//pagespeed/apache:apache_core",
        "//test/net/instaweb/rewriter:test_base",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/util:util_test_base",
    ],
)

pagespeed_cc_test(
    name = "apache",
    srcs = [
        "apache_config_test.cc",
        "apache_fetch_test.cc",
        "apache_mmap_bucket_test.cc",
        "apache_mpm_detection_test.cc",
        "apache_writer_test.cc",
        "daemon_seam_test.cc",
        "header_util_test.cc",
        "in_place_not_modified_vary_fetch_test.cc",
        "simple_buffered_apache_fetch_test.cc",
    ],
    copts = [
        "-I/usr/include/apache2/",
        "-Iexternal/aprutil/include/",
        "-Iexternal/aprutil/include/arch/unix/",
        "-Iexternal/aprutil/",
    ] + select({
        "//bazel:linux_arm64": [
            "-Ithird_party/aprutil/gen/arch/linux/arm64/include/",
            "-Ithird_party/apr/gen/arch/linux/arm64/include/",
        ],
        "//conditions:default": [
            "-Ithird_party/aprutil/gen/arch/linux/x64/include/",
            "-Ithird_party/apr/gen/arch/linux/x64/include/",
        ],
    }) + [
        "-Iexternal/apr/include/",
        "-Iexternal/apr/include/arch/unix/",
        "-Iexternal/apr/",
    ],
    data = [
        # the design record: apache_mpm_detection_test.cc reads this source to pin the
        # FinalizeThreadCounts() call site in the post-config hook.
        # daemon_seam_test.cc reads mod_instaweb.cc and instaweb_handler.cc for
        # the same reason: both call sites need a live httpd to execute.
        "//pagespeed/apache:apache_sources",
        "//pagespeed/apache:instaweb_handler_src",
        "//pagespeed/apache:mod_instaweb_src",
        "//test/net/instaweb/rewriter:testdata",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/system:optimization_thread_policy",
        "//test/pagespeed/apache:test_base",
    ],
)

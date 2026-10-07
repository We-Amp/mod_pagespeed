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
        "apache_host_names_test.cc",
        "apache_mmap_bucket_test.cc",
        "apache_mpm_detection_test.cc",
        "apache_writer_test.cc",
        "daemon_seam_test.cc",
        "header_util_test.cc",
        "in_place_not_modified_vary_fetch_test.cc",
        "instaweb_context_test.cc",
        "log_message_handler_test.cc",
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
        # apache_mpm_detection_test.cc reads this source to pin the
        # FinalizeThreadCounts() call site in the post-config hook.
        # daemon_seam_test.cc reads mod_instaweb.cc, instaweb_handler.cc and
        # system_rewrite_driver_factory.cc for the same reason: those call
        # sites need a live httpd to execute.
        # instaweb_context_test.cc reads instaweb_context.cc (in
        # apache_sources) to pin where the property-cache lookup takes the
        # device class from.
        "//pagespeed/apache:apache_sources",
        "//pagespeed/apache:instaweb_handler_src",
        "//pagespeed/apache:mod_instaweb_src",
        "//pagespeed/system:system_rewrite_driver_factory_src",
        "//test/net/instaweb/rewriter:testdata",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        # log_message_handler_test.cc includes spdlog sinks directly.
        "@spdlog//:spdlog",
        "//pagespeed/system:optimization_thread_policy",
        "//test/pagespeed/apache:test_base",
    ],
)

# The static asset name lookup: both readings of the request path must agree
# and name a file directly under the static asset prefix.
pagespeed_cc_test(
    name = "apache_static_asset_path_test",
    srcs = ["apache_static_asset_path_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/apache:apache_static_asset_path",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

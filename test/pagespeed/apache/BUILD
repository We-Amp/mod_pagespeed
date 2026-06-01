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
        "-Ithird_party/aprutil/gen/arch/linux/x64/include/",
        "-Ithird_party/apr/gen/arch/linux/x64/include/",
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
        "apache_writer_test.cc",
        "header_util_test.cc",
        "simple_buffered_apache_fetch_test.cc",
    ],
    copts = [
        "-I/usr/include/apache2/",
        "-Iexternal/aprutil/include/",
        "-Iexternal/aprutil/include/arch/unix/",
        "-Iexternal/aprutil/",
        "-Ithird_party/aprutil/gen/arch/linux/x64/include/",
        "-Ithird_party/apr/gen/arch/linux/x64/include/",
        "-Iexternal/apr/include/",
        "-Iexternal/apr/include/arch/unix/",
        "-Iexternal/apr/",
    ],
    data = ["//test/net/instaweb/rewriter:testdata"],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/apache:test_base",
    ],
)

# APR-dependent system tests (moved from test/pagespeed/system/).
# Tests APR-based memcached client implementation.
pagespeed_cc_test(
    name = "apache_apr",
    srcs = [
        "apr_mem_cache_test.cc",
    ],
    copts = [
        "-Iexternal/aprutil/include/",
        "-Iexternal/aprutil/include/arch/unix/",
        "-Iexternal/aprutil/",
        "-Ithird_party/aprutil/gen/arch/linux/x64/include/",
        "-Ithird_party/apr/gen/arch/linux/x64/include/",
        "-Iexternal/apr/include/",
        "-Iexternal/apr/include/arch/unix/",
        "-Iexternal/apr/",
    ],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/apache:apache_apr",
        "//test/pagespeed/system:test_base",
    ],
)

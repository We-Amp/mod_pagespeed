load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

# Linker version script to hide internal symbols (especially BoringSSL)
# from the shared library, preventing conflicts with mod_ssl's OpenSSL.
exports_files(
    ["mod_pagespeed.lds"],
    visibility = ["//visibility:public"],
)

# Apache-specific system test scripts
filegroup(
    name = "system_test_files",
    srcs = [
        "gzip_test.sh",
        "process_scope_test.sh",
        "system_test.sh",
    ] + glob(["system_tests/*.sh"]),
    visibility = ["//visibility:public"],
)

# APR initialization utilities and thread-compatible pool.
# (The APR memcached client lived here but was removed once the live
# memcached path moved to libmemcached via //pagespeed/system MemcachedCache;
# these remaining components use only @apr core, not @aprutil.)
cc_library(
    name = "apache_apr",
    srcs = [
        "apr_init.cc",
        "apr_thread_compatible_pool.cc",
    ],
    hdrs = [
        "apr_init.h",
        "apr_thread_compatible_pool.h",
    ],
    copts = select({
        "//bazel:linux_arm64": ["-Ithird_party/apr/gen/arch/linux/arm64/include/"],
        "//conditions:default": ["-Ithird_party/apr/gen/arch/linux/x64/include/"],
    }) + [
        "-Iexternal/apr/include/",
        "-Iexternal/apr/include/arch/unix/",
        "-Iexternal/apr/",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/system:system_apache",
        "@apr",
    ],
)

# Core Apache library without mod_instaweb.cc.
# Use this for tests to avoid the static ApacheProcessContext in mod_instaweb.cc
# which conflicts with test infrastructure's RewriteTestBaseProcessContext.
cc_library(
    name = "apache_core",
    srcs = [
        "apache_config.cc",
        "apache_fetch.cc",
        "apache_message_handler.cc",
        "apache_mmap_bucket.cc",
        "apache_request_context.cc",
        "apache_rewrite_driver_factory.cc",
        "apache_server_context.cc",
        "apache_slurp.cc",
        "apache_thread_system.cc",
        "apache_writer.cc",
        "apr_timer.cc",
        "header_util.cc",
        "instaweb_context.cc",
        "instaweb_handler.cc",
        "log_message_handler.cc",
        "simple_buffered_apache_fetch.cc",
        "streaming_pagespeed_resource_fetch.cc",
    ],
    hdrs = [
        "apache_config.h",
        "apache_fetch.h",
        "apache_httpd_includes.h",
        "apache_logging_includes.h",
        "apache_message_handler.h",
        "apache_mmap_bucket.h",
        "apache_request_context.h",
        "apache_rewrite_driver_factory.h",
        "apache_server_context.h",
        "apache_thread_system.h",
        "apache_writer.h",
        "apr_timer.h",
        "header_util.h",
        "instaweb_context.h",
        "instaweb_handler.h",
        "log_message_handler.h",
        "mod_instaweb.h",
        "simple_buffered_apache_fetch.h",
        "streaming_pagespeed_resource_fetch.h",
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
    visibility = ["//visibility:public"],
    deps = [
        ":apache_apr",
        "//pagespeed/automatic",
        "//pagespeed/kernel/thread:event_scheduler",
        "//pagespeed/kernel/thread:libevent_dispatcher",
        "//pagespeed/system:system_apache",
        "//pagespeed/system:curl_fetcher",
        # @aprutil supplies the bucket-brigade glue (apr_brigade_create in
        # instaweb_context.cc) and the httpd util_filter API; it is no longer
        # for memcached. Declared here because apache_apr no longer pulls it.
        "@aprutil",
    ],
)

# Full Apache library including mod_instaweb.cc (Apache module entry point).
# This includes a static ApacheProcessContext that initializes the module.
# For tests, use :apache_core instead to avoid ProcessContext conflicts.
cc_library(
    name = "apache",
    srcs = [
        "mod_instaweb.cc",
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
    alwayslink = True,
    visibility = ["//visibility:public"],
    deps = [
        ":apache_core",
    ],
)

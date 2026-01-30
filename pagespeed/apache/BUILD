load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])  # Apache 2

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

# APR-dependent components: APR memcached client,
# APR initialization utilities, and thread-compatible pool.
cc_library(
    name = "apache_apr",
    srcs = [
        "apr_init.cc",
        "apr_mem_cache.cc",
        "apr_thread_compatible_pool.cc",
    ],
    hdrs = [
        "apr_init.h",
        "apr_mem_cache.h",
        "apr_thread_compatible_pool.h",
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
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/system",
        "@apr",
        "@aprutil",
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
    ],
    hdrs = [
        "apache_config.h",
        "apache_fetch.h",
        "apache_httpd_includes.h",
        "apache_logging_includes.h",
        "apache_message_handler.h",
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
    visibility = ["//visibility:public"],
    deps = [
        ":apache_apr",
        "//pagespeed/automatic",
        "//pagespeed/kernel/thread:event_scheduler",
        "//pagespeed/kernel/thread:libevent_dispatcher",
        "//pagespeed/system",
        "//pagespeed/system:curl_fetcher",
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
        "-Ithird_party/aprutil/gen/arch/linux/x64/include/",
        "-Ithird_party/apr/gen/arch/linux/x64/include/",
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

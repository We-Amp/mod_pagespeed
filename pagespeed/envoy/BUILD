load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_cc_test",
)
load("@envoy_api//bazel:api_build_system.bzl", "api_cc_py_proto_library")

package(default_visibility = ["//visibility:public"])

# Common compiler options for Envoy integration code.
# These suppress warnings from Envoy headers and PageSpeed/Envoy interface code.
ENVOY_COPTS = [
    "-Wno-unused-parameter",
    "-Wno-old-style-cast",
    "-Wno-macro-redefined",
]

api_cc_py_proto_library(
    name = "http_filter_proto",
    srcs = ["http_filter.proto"],
)

envoy_cc_library(
    name = "http_filter_lib",
    srcs = [
        "envoy_admin_fetch.cc",
        "envoy_async_fetch.cc",
        "envoy_base_fetch.cc",
        "envoy_message_handler.cc",
        "envoy_metrics_collector.cc",
        "envoy_process_context.cc",
        "envoy_rewrite_driver_factory.cc",
        "envoy_rewrite_options.cc",
        "envoy_server_context.cc",
        "envoy_vhost_config.cc",
        "http_filter.cc",
        "log_message_handler.cc",
    ],
    hdrs = [
        "envoy_admin_fetch.h",
        "envoy_async_fetch.h",
        "envoy_base_fetch.h",
        "envoy_message_handler.h",
        "envoy_metrics_collector.h",
        "envoy_process_context.h",
        "envoy_rewrite_driver_factory.h",
        "envoy_rewrite_options.h",
        "envoy_server_context.h",
        "envoy_thread_system.h",
        "envoy_vhost_config.h",
        "header_utils.h",
        "http_filter.h",
        "log_message_handler.h",
    ],
    copts = ENVOY_COPTS + [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",  # Envoy uses Redis only
    ],
    repository = "@envoy",
    visibility = ["//visibility:public"],
    deps = [
        ":envoy_dispatcher_adapter",
        ":http_filter_proto_cc_proto",
        "//pagespeed/automatic",
        "//pagespeed/kernel/thread:event_scheduler",
        "//pagespeed/kernel/util",
        "//pagespeed/system:system_envoy",  # Minimal system lib (no memcached, no fork)
        "//pagespeed/system:circuit_breaker",
        "//pagespeed/system:circuit_breaker_fetcher",
        "//pagespeed/system:curl_fetcher",
        # Minimal Envoy deps - use filter_config_interface instead of main_common_lib
        # to avoid pulling in QUIC, HTTP/3, gRPC implementations (reduces from 137 to 52 packages)
        "@envoy//envoy/server:filter_config_interface",
        "@envoy//source/common/common:minimal_logger_lib_with_external_headers",
        # For admin authentication with CIDR range parsing
        "@envoy//source/common/network:cidr_range_lib",
        "@envoy//source/common/network:utility_lib",
        "@boringssl//:ssl",
    ],
)

envoy_cc_library(
    name = "http_filter_config",
    srcs = ["http_filter_config.cc"],
    copts = ENVOY_COPTS,
    repository = "@envoy",
    deps = [
        ":http_filter_lib",
        "@envoy//envoy/server:filter_config_interface",
        "@envoy//source/common/common:minimal_logger_lib_with_external_headers",
    ],
)

# EnvoyDispatcherAdapter wraps Envoy's native dispatcher for PageSpeed.
# This allows PageSpeed to use Envoy's event loop directly without a separate
# SchedulerThread.
envoy_cc_library(
    name = "envoy_dispatcher_adapter",
    srcs = ["envoy_dispatcher_adapter.cc"],
    hdrs = ["envoy_dispatcher_adapter.h"],
    copts = ENVOY_COPTS,
    repository = "@envoy",
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/thread:event_dispatcher",
        "@envoy//envoy/event:dispatcher_interface_with_external_headers",
        "@envoy//envoy/event:timer_interface_with_external_headers",
        "@mod_pagespeed//base:log_shim",
    ],
)

# Shared library that can be loaded by a standard Envoy binary.
# Build with: bazel build --config=clang-libstdcxx13 //pagespeed/envoy:pagespeed_filter.so
#
# This produces a smaller artifact (~50MB) compared to the full envoy_pagespeed binary (~200MB).
# The filter registers itself via static initialization when the .so is dlopen'd.
#
# Usage with standard Envoy:
#   1. Copy pagespeed_filter.so to your Envoy container/deployment
#   2. Use --extension-config-dir or LD_PRELOAD to load it
#   3. Configure the "pagespeed" filter in your Envoy config
#
# Note: The .so and Envoy binary must be built with compatible compiler/flags.
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "pagespeed_filter.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        ":http_filter_config",
    ],
)

# Standalone Envoy binary with PageSpeed filter
# Build with: bazel build --config=clang-libstdcxx13 //pagespeed/envoy:envoy_pagespeed
# Note: Building Envoy requires significant memory (16GB+ recommended)
load("@envoy//bazel:envoy_build_system.bzl", "envoy_cc_binary")

envoy_cc_binary(
    name = "envoy_pagespeed",
    repository = "@envoy",
    deps = [
        ":http_filter_config",
        "@envoy//source/exe:envoy_main_entry_lib",
        # Required cluster types
        "@envoy//source/extensions/clusters/static:static_cluster_lib",
        # Required for HTTP connection manager
        "@envoy//source/extensions/filters/network/http_connection_manager:config",
        # Gzip compression support
        "@envoy//source/extensions/filters/http/compressor:config",
        "@envoy//source/extensions/compression/gzip/compressor:config",
    ],
)

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//bazel:closure_compiler.bzl", "closure_compiler_gen")
load("//bazel:data2c.bzl", "data2c_gen2_admin_css", "data2c_gen2_admin_js")

licenses(["notice"])  # Apache 2

closure_compiler_gen(
    name = "caches_dbg",
    entry_points = [
        "goog:pagespeed.Caches",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/caches.js",
    opt = False,
)

closure_compiler_gen(
    name = "caches_opt",
    entry_points = [
        "goog:pagespeed.Caches",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/caches.js",
    opt = True,
)

closure_compiler_gen(
    name = "console_opt",
    entry_points = [
        "goog:pagespeed.Console",
        "goog:pagespeed.statistics",
    ],
    externs = [
        "pagespeed/system/js_externs.js",
        "third_party/closure/externs/google_visualization_api.js",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/console.js",
    opt = True,
)

closure_compiler_gen(
    name = "console_dbg",
    entry_points = [
        "goog:pagespeed.Console",
        "goog:pagespeed.statistics",
    ],
    externs = [
        "pagespeed/system/js_externs.js",
        "third_party/closure/externs/google_visualization_api.js",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/console.js",
    opt = False,
)

closure_compiler_gen(
    name = "graphs_dbg",
    entry_points = [
        "goog:pagespeed.Graphs",
    ],
    externs = [
        "third_party/closure/externs/google_visualization_api.js",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/graphs.js",
    opt = False,
)

closure_compiler_gen(
    name = "graphs_opt",
    entry_points = [
        "goog:pagespeed.Graphs",
    ],
    externs = [
        "third_party/closure/externs/google_visualization_api.js",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/graphs.js",
    opt = True,
)

closure_compiler_gen(
    name = "messages_dbg",
    entry_points = [
        "goog:pagespeed.Messages",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/messages.js",
    opt = False,
)

closure_compiler_gen(
    name = "messages_opt",
    entry_points = [
        "goog:pagespeed.Messages",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/messages.js",
    opt = True,
)

closure_compiler_gen(
    name = "statistics_dbg",
    entry_points = [
        "goog:pagespeed.Statistics",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/statistics.js",
    opt = False,
)

closure_compiler_gen(
    name = "statistics_opt",
    entry_points = [
        "goog:pagespeed.Statistics",
    ],
    js_dir = "system",
    js_src = "pagespeed/system/statistics.js",
    opt = True,
)

data2c_gen2_admin_js(
    name = "data2c_generated",
    srcs = [
        "caches",
        "console",
        "graphs",
        "messages",
        "statistics",
    ],
    opt = False,
)

data2c_gen2_admin_js(
    name = "data2c_generated",
    srcs = [
        "caches",
        "console",
        "graphs",
        "messages",
        "statistics",
    ],
    opt = True,
)

data2c_gen2_admin_css(
    name = "data2c_generated",
    srcs = [
        "admin_site",
        "caches",
        "console",
        "graphs",
        "statistics",
    ],
    opt = False,
)

data2c_gen2_admin_css(
    name = "data2c_generated",
    srcs = [
        "admin_site",
        "caches",
        "console",
        "graphs",
        "statistics",
    ],
    opt = True,
)

# =============================================================================
# Fine-grained cache backend targets for faster incremental builds
# =============================================================================

# Shared utility for external server specifications (Redis/Memcached).
# No external dependencies - can be used independently.
cc_library(
    name = "external_server_spec",
    srcs = ["external_server_spec.cc"],
    hdrs = ["external_server_spec.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Redis cache backend. Depends on hiredis.
cc_library(
    name = "redis_cache",
    srcs = ["redis_cache.cc"],
    hdrs = ["redis_cache.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "//pagespeed/kernel/thread",
        "//third_party/redis-crc",
        "@hiredis",
    ],
)

# Memcached cache backend. Depends on libmemcached.
cc_library(
    name = "memcached_cache",
    srcs = ["memcached_cache.cc"],
    hdrs = ["memcached_cache.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache",
        "@libmemcached",
    ],
)

# =============================================================================
# Cache backend shared libraries (for dynamic loading)
# =============================================================================
# These can be loaded at runtime using dlopen/dlsym for configurations that
# need specific cache backends without linking them into the main binary.

load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "redis_cache.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":redis_cache"],
)

cc_binary(
    name = "memcached_cache.so",
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":memcached_cache"],
)

# =============================================================================
# Envoy-optimized system library
# =============================================================================

# Minimal system library for Envoy deployments.
# Excludes components not needed by Envoy:
#   - controller_manager.cc (Envoy handles process management)
#   - memcached_cache.cc (Redis is the preferred external cache)
# This reduces binary size and external dependencies.
cc_library(
    name = "system_envoy",
    srcs = [
        "add_headers_fetcher.cc",
        "admin_site.cc",
        "external_server_spec.cc",
        "in_place_resource_recorder.cc",
        "loopback_route_fetcher.cc",
        "redis_cache.cc",
        "system_cache_path.cc",
        "system_caches.cc",
        "system_message_handler.cc",
        "system_request_context.cc",
        "system_rewrite_driver_factory.cc",
        "system_rewrite_options.cc",
        "system_server_context.cc",
        "system_thread_system.cc",
        ":css_data2c_generated_admin_site",
        ":css_data2c_generated_admin_site_opt",
        ":css_data2c_generated_caches",
        ":css_data2c_generated_caches_opt",
        ":css_data2c_generated_console",
        ":css_data2c_generated_console_opt",
        ":css_data2c_generated_graphs",
        ":css_data2c_generated_graphs_opt",
        ":css_data2c_generated_statistics",
        ":css_data2c_generated_statistics_opt",
        ":js_data2c_generated_caches",
        ":js_data2c_generated_caches_opt",
        ":js_data2c_generated_console",
        ":js_data2c_generated_console_opt",
        ":js_data2c_generated_graphs",
        ":js_data2c_generated_graphs_opt",
        ":js_data2c_generated_messages",
        ":js_data2c_generated_messages_opt",
        ":js_data2c_generated_statistics",
        ":js_data2c_generated_statistics_opt",
    ],
    hdrs = [
        "add_headers_fetcher.h",
        "admin_site.h",
        "controller_manager.h",
        "controller_process.h",
        "external_server_spec.h",
        "in_place_resource_recorder.h",
        "loopback_route_fetcher.h",
        "redis_cache.h",
        "system_cache_path.h",
        "system_caches.h",
        "system_message_handler.h",
        "system_request_context.h",
        "system_rewrite_driver_factory.h",
        "system_rewrite_options.h",
        "system_server_context.h",
        "system_thread_system.h",
    ],
    copts = [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
        "-DPAGESPEED_NO_FORK",  # Use stub ControllerManager
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        ":redis_cache",
        ":system_controller_process_h",
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/cache:cyclone_cache",
        "//pagespeed/kernel/sharedmem",
        "//third_party/redis-crc",
        "@hiredis",
    ],
)

# =============================================================================
# Windows-compatible system library
# =============================================================================

# Minimal system library for Windows cross-compilation.
# Excludes POSIX-only components:
#   - controller_manager.cc (uses fork/signals)
#   - memcached_cache.cc (libmemcached is POSIX-only)
# Use this target for Windows IIS integration.
cc_library(
    name = "system_windows",
    srcs = [
        "add_headers_fetcher.cc",
        "admin_site.cc",
        "external_server_spec.cc",
        "in_place_resource_recorder.cc",
        "loopback_route_fetcher.cc",
        "redis_cache.cc",
        "system_cache_path.cc",
        "system_caches.cc",
        "system_message_handler.cc",
        "system_request_context.cc",
        "system_rewrite_driver_factory.cc",
        "system_rewrite_options.cc",
        "system_server_context.cc",
        "system_thread_system.cc",
        ":css_data2c_generated_admin_site",
        ":css_data2c_generated_admin_site_opt",
        ":css_data2c_generated_caches",
        ":css_data2c_generated_caches_opt",
        ":css_data2c_generated_console",
        ":css_data2c_generated_console_opt",
        ":css_data2c_generated_graphs",
        ":css_data2c_generated_graphs_opt",
        ":css_data2c_generated_statistics",
        ":css_data2c_generated_statistics_opt",
        ":js_data2c_generated_caches",
        ":js_data2c_generated_caches_opt",
        ":js_data2c_generated_console",
        ":js_data2c_generated_console_opt",
        ":js_data2c_generated_graphs",
        ":js_data2c_generated_graphs_opt",
        ":js_data2c_generated_messages",
        ":js_data2c_generated_messages_opt",
        ":js_data2c_generated_statistics",
        ":js_data2c_generated_statistics_opt",
    ],
    hdrs = [
        "add_headers_fetcher.h",
        "admin_site.h",
        "controller_manager.h",
        "controller_process.h",
        "external_server_spec.h",
        "in_place_resource_recorder.h",
        "loopback_route_fetcher.h",
        "redis_cache.h",
        "system_cache_path.h",
        "system_caches.h",
        "system_message_handler.h",
        "system_request_context.h",
        "system_rewrite_driver_factory.h",
        "system_rewrite_options.h",
        "system_server_context.h",
        "system_thread_system.h",
    ],
    copts = [
        "-DPAGESPEED_ENABLE_MEMCACHED=0",
        "-DPAGESPEED_NO_FORK",  # Redundant with _WIN32, but explicit for clarity
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        ":redis_cache",
        ":system_controller_process_h",
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/cache:cyclone_cache",
        "//pagespeed/kernel/sharedmem",
        "//third_party/redis-crc",
        "@hiredis",
    ],
)

# =============================================================================
# Main system library (backward-compatible)
# =============================================================================

# Core system library - APR-free.
# Used by both Apache and Envoy deployments.
cc_library(
    name = "system",
    srcs = [
        "add_headers_fetcher.cc",
        "admin_site.cc",
        "controller_manager.cc",
        "external_server_spec.cc",
        "in_place_resource_recorder.cc",
        "loopback_route_fetcher.cc",
        "memcached_cache.cc",
        "redis_cache.cc",
        "system_cache_path.cc",
        "system_caches.cc",
        "system_message_handler.cc",
        "system_request_context.cc",
        "system_rewrite_driver_factory.cc",
        "system_rewrite_options.cc",
        "system_server_context.cc",
        "system_thread_system.cc",
        ":css_data2c_generated_admin_site",
        ":css_data2c_generated_admin_site_opt",
        ":css_data2c_generated_caches",
        ":css_data2c_generated_caches_opt",
        ":css_data2c_generated_console",
        ":css_data2c_generated_console_opt",
        ":css_data2c_generated_graphs",
        ":css_data2c_generated_graphs_opt",
        ":css_data2c_generated_statistics",
        ":css_data2c_generated_statistics_opt",
        ":js_data2c_generated_caches",
        ":js_data2c_generated_caches_opt",
        ":js_data2c_generated_console",
        ":js_data2c_generated_console_opt",
        ":js_data2c_generated_graphs",
        ":js_data2c_generated_graphs_opt",
        ":js_data2c_generated_messages",
        ":js_data2c_generated_messages_opt",
        ":js_data2c_generated_statistics",
        ":js_data2c_generated_statistics_opt",
    ],
    hdrs = [
        "add_headers_fetcher.h",
        "admin_site.h",
        "controller_manager.h",
        "controller_process.h",
        "external_server_spec.h",
        "in_place_resource_recorder.h",
        "loopback_route_fetcher.h",
        "memcached_cache.h",
        "redis_cache.h",
        "system_cache_path.h",
        "system_caches.h",
        "system_message_handler.h",
        "system_request_context.h",
        "system_rewrite_driver_factory.h",
        "system_rewrite_options.h",
        "system_server_context.h",
        "system_thread_system.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":external_server_spec",
        ":memcached_cache",
        ":redis_cache",
        ":system_controller_process_h",
        "//net/instaweb/http",
        "//net/instaweb/rewriter",
        "//pagespeed/kernel/cache:cyclone_cache",
        "//pagespeed/kernel/sharedmem",
        "//third_party/redis-crc",
        "@hiredis",
        "@libmemcached",
    ],
)


cc_library(
    name = "curl_fetcher",
    srcs = [
        "curl_fetch.cc",
        "curl_url_async_fetcher.cc",
    ],
    hdrs = [
        "curl_fetch.h",
        "curl_url_async_fetcher.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//net/instaweb/http",
        "//net/instaweb:net_instaweb_lib",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http",
        "@curl",
    ],
)

cc_library(
    name = "system_controller_process_h",
    srcs = [
    ],
    hdrs = [
        "controller_process.h",
    ],
    visibility = ["//visibility:public"],
)

# System-level test scripts for Apache/Nginx system tests
filegroup(
    name = "system_test_files",
    srcs = [
        "system_test.sh",
    ] + glob(["system_tests/*.sh"]),
    visibility = ["//visibility:public"],
)

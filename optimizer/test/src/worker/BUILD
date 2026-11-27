# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "llms_txt_builder_test",
    deps = [
        "//src/worker:llms_txt_builder",
    ],
)

gtest_test(
    name = "subresource_gate_test",
    deps = [
        "//lib/classify:alternate_metadata",
        "//lib/net:upstream_pin",
        "//src/browser:agent_fetcher",
        "//src/worker:subresource_gate",
    ],
)

gtest_test(
    name = "serve_stats_test",
    # Production code already has Windows mmap support; test uses std::atomic_ref (C++20).
    deps = [
        "//lib/base:message_handler",
        "//lib/classify:content_type",
        "//src/worker:serve_stats",
    ],
)

gtest_test(
    name = "syscall_filter_test",
    deps = [
        "//src/worker:syscall_filter",
    ],
)

# One source, both build variants.  The default target links
# the SHIPPED library (the switch compiled out); the _enabled one links the
# local_defines = PAGESPEED_SELFTEST variant.  Building both on every run is
# what keeps the #ifdef'd half compiling, and asserting both is what proves
# the shipped half really does not carry the switch.
gtest_test(
    name = "windows_service_test",
    deps = [
        "//src/worker:windows_service",
    ],
)

gtest_test(
    name = "pipe_dacl_test",
    linkopts = select({
        "@platforms//os:windows": ["advapi32.lib"],
        "//conditions:default": [],
    }),
    deps = [
        "//src/worker:pipe_dacl",
    ],
)

gtest_test(
    name = "syscall_selftest_test",
    deps = [
        "//src/worker:syscall_selftest",
    ],
)

gtest_test(
    name = "syscall_selftest_enabled_test",
    srcs = ["syscall_selftest_test.cc"],
    deps = [
        "//src/worker:syscall_selftest_enabled",
    ],
)

gtest_test(
    name = "html_scanner_test",
    deps = [
        "//src/worker:html_scanner",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "cascade_layer_order_test",
    deps = [
        "//src/worker:cascade_layer_order",
    ],
)

gtest_test(
    name = "layer_order_sources_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//src/worker:cascade_layer_order",
        "//src/worker:html_scanner",
        "//src/worker:layer_order_sources",
    ],
)

gtest_test(
    name = "html_css_injector_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_writer",
        "//lib/html:compat",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_node",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//src/worker:html_css_injector",
    ],
)

gtest_test(
    name = "noscript_strip_test",
    deps = [
        "//src/worker:html_scanner",
        "//src/worker:noscript_strip",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "css_cache_inliner_test",
    deps = [
        "//src/worker:css_cache_inliner",
    ],
)

gtest_test(
    name = "analysis_resource_map_test",
    deps = [
        "//lib/classify:url_normalizer",
        "//src/worker:analysis_resource_map",
        "@com_google_absl//absl/container:flat_hash_map",
    ],
)

gtest_test(
    name = "critical_css_extractor_test",
    # The wide-replaced-element cases build large fixtures and take 30+ s
    # each under TSan on a slow or busy machine; the default "small" 60 s
    # budget timed the whole suite out there, while the same suite takes
    # 12 s on a fast one.
    size = "medium",
    deps = [
        "//lib/classify:capability_mask",
        "//src/worker:cascade_layer_order",
        "//src/worker:critical_css_extractor",
        "//src/worker:html_css_injector",
    ],
)

gtest_test(
    name = "unsafe_force_async_css_test",
    deps = [
        "//src/worker:unsafe_force_async_css",
    ],
)

gtest_test(
    name = "font_url_extractor_test",
    deps = [
        "//src/worker:font_url_extractor",
    ],
)

gtest_test(
    name = "html_transform_filter_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_writer",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//lib/html:compat",
        "//lib/html:html_keywords",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//src/worker:cascade_layer_order",
        "//src/worker:html_transform_filter",
        "//src/worker:subresource_gate",
    ],
)

gtest_test(
    name = "image_transcoder_test",
    size = "large",
    data = ["//test/lib/image:testdata"],
    # ~850 s single-process on the Windows lane against a 900 s ceiling; the
    # cases are independent codec round-trips, so shard them.  A handful of
    # cases take minutes each (the size-gate matrices), and gtest deals tests
    # to shards round-robin by index, so the count is sized to keep them on
    # separate shards: at 4 shards the slowest held 720 s of a 416 s mean.
    shard_count = 8,
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//lib/classify:capability_mask",
        "//lib/image:exif_orientation",
        "//lib/image:image_util",
        "//lib/image:jpeg",
        "//lib/image:quality_verifier",
        "//lib/image:read_image",
        "//src/worker:image_transcoder",
        "@libavif",
        "@libwebp//:webp_encode",
    ],
)

gtest_test(
    name = "optimization_policy_test",
    deps = [
        "//src/browser:optimization_profile",
        "//src/worker:optimization_policy",
    ],
)

gtest_test(
    name = "config_file_test",
    deps = [
        "//src/worker:config_file",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "html_mode_test",
    deps = [
        "//src/worker:html_mode",
    ],
)

gtest_test(
    name = "shared_config_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:shared_config",
        "@cyclone",
    ],
)

gtest_test(
    name = "cache_dir_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:cache_dir",
    ],
)

gtest_test(
    name = "cpu_budget_test",
    deps = [
        "//src/worker:cpu_budget",
    ],
)

gtest_test(
    name = "webbotauth_warmer_test",
    deps = [
        "//src/crypto/webbotauth:webbotauth_core",
        "//src/worker:webbotauth_warmer",
        "//test/test_util:temp_dir",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "text_compressor_test",
    deps = [
        "//src/worker:text_compressor",
    ],
)

gtest_test(
    name = "config_persister_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:config_persister",
        "@libuv",
    ],
)

gtest_test(
    name = "http_server_test",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:http_server",
        "//test/test_util:tcp_client",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "cache_handlers_test",
    size = "medium",
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:content_type",
        "//src/worker:cache_handlers",
        "//src/worker:known_sites",
        "//src/worker:url_registry",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "static_file_handler_test",
    # TSan: SecurityHeadersDisabled flakes due to server-restart timing under
    # TSan's ~10x slowdown. Not a real race — needs proper sync, not timeouts.
    tags = ["no_tsan"],
    deps = [
        "//lib/base:message_handler",
        "//src/worker:static_file_handler",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "@libuv",
    ],
)

# integration test (starts the worker); medium gives it 300 s
gtest_test(
    name = "websocket_test",
    size = "medium",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:websocket",
        "//src/worker:ws_handlers",
        "//test/test_util:scoped_thread_join",
        "//test/test_util:tcp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "json_output_test",
    # :all_sources materializes the package's current *.cc/*.h files, by
    # relative path, under this test's working directory -- the same
    # convention test/lib/image:png_optimizer_test relies on for its
    # testdata/ -- so the no-bare-dump gate can scan them.
    data = ["//src/worker:all_sources"],
    deps = [
        "//lib/base:message_handler",
        "//src/worker:cache_handlers",
        "//src/worker:http_server",
        "//src/worker:json_dump",
        "//src/worker:url_registry",
        "//src/worker:ws_handlers",
        "//test/test_util:tcp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "log_handlers_test",
    size = "medium",
    data = ["testdata/logs_page.golden.json"],
    deps = [
        "//lib/base:message_handler",
        "//src/product_version:version",
        "//src/worker:api_handlers",
        "//src/worker:http_server",
        "//src/worker:serve_stats",
        "//src/worker:shared_config",
        "//src/worker:worker_types",
        "//src/worker:ws_handlers",
        "//test/test_util:scoped_thread_join",
        "//test/test_util:tcp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "capture_handlers_test",
    deps = [
        "//lib/base:message_handler",
        "//src/browser:device_emulation",
        "//src/worker:capture_handlers",
        "//test/test_util:cdp_scripted_peer",
        "//test/test_util:tcp_client",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "api_handlers_test",
    data = ["testdata/stats_page.golden.json"],
    deps = [
        "//lib/base:message_handler",
        "//src/product_version:version",
        "//src/worker:api_handlers",
        "//src/worker:config_file",
        "//src/worker:config_persister",
        "//src/worker:serve_stats",
        "//src/worker:shared_config",
        "//src/worker:worker_types",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

# integration test (starts the worker); medium gives it 300 s
gtest_test(
    name = "worker_api_test",
    size = "medium",
    deps = [
        "//lib/base:message_handler",
        "//src/worker:serve_stats",
        "//src/worker:worker_lib",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
    ],
)

# Runs the real daemon binary as a child process: what it pins (a peer that
# closes early costs one connection, never the process) is decided by the
# binary's own start-up, not by anything an in-process Worker shares with the
# test.
gtest_test(
    name = "worker_client_disconnect_test",
    size = "medium",
    data = ["//src/worker:factory_worker"],
    env = {"FACTORY_WORKER": "$(rootpath //src/worker:factory_worker)"},
    deps = [
        "//src/proto:worker_ipc",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "url_registry_test",
    deps = [
        "//src/worker:known_sites",
        "//src/worker:url_registry",
    ],
)

gtest_test(
    name = "known_sites_file_test",
    deps = [
        "//src/worker:known_sites",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "worker_test",
    size = "large",
    # One target, many topic files: the single 21k-line translation unit this
    # replaced dominated both compile and lint time. The target name is
    # unchanged, so exclusions by name keep working.
    srcs = [
        "worker_agent_markdown_test.cc",
        "worker_cooldown_purge_test.cc",
        "worker_critical_css_test.cc",
        "worker_css_js_test.cc",
        "worker_dedup_heal_test.cc",
        "worker_durable_original_test.cc",
        "worker_html_test.cc",
        "worker_image_siblings_test.cc",
        "worker_image_test.cc",
        "worker_lifecycle_test.cc",
        "worker_mgmt_test.cc",
        "worker_notify_test.cc",
        "worker_origin_refresh_test.cc",
        "worker_peer_rewrite_test.cc",
        "worker_subresource_gate_test.cc",
        "worker_test_fixture.cc",
        "worker_test_fixture.h",
    ],
    # TSan runs the full proactive image matrices (no early-exit polls left),
    # which under the ~10x slowdown can push a shard past the 900s "large"
    # cap on slower hosts; the sanitizer-scaled failure-path poll ceilings
    # also need headroom to report a real failure instead of a timeout.
    # NOTE: bazel has no per-config timeout without a sanitizer
    # config_setting (repo has none yet), so this widening applies to ALL
    # lanes: a genuinely hung worker reports at 3600s instead of 900s.
    timeout = "eternal",
    # A few replays of the full variant matrix take 4-8 minutes each; at 8
    # shards two of them shared a shard (slowest 581 s against a 316 s
    # mean).  16 keeps them apart; the bound is now the longest single case.
    data = ["//test/lib/image:testdata"],
    shard_count = 16,
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:freshness",
        "//lib/cache:html_withheld_marker",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:option_context",
        "//lib/net:upstream_pin",
        "//src/browser:optimization_profile",
        "//src/browser:template_detector",
        # Early Hints line parser: the test asserts the sentinel a deferred
        # stylesheet produces parses to rel=preload; as=style, which is the
        # contract the 103 emitters consume. Pure C++, no nginx headers.
        "//src/nginx:early_hints_util",
        "//src/nginx:etag_util",
        "//src/proto:worker_ipc",
        "//src/worker:browser_analysis_manager",
        "//src/worker:cascade_layer_order",
        "//src/worker:critical_css_extractor",
        "//src/worker:html_scanner",
        "//src/worker:layer_order_sources",
        "//src/worker:shared_config",
        "//src/worker:known_sites",
        "//src/worker:subresource_gate",
        "//src/worker:text_compressor",
        "//src/worker:url_registry",
        "//src/worker:worker_lib",
        "@com_google_absl//absl/strings",
        "@libavif",
        "//test/test_util:cache_test_peer",
        "//test/test_util:pipe_client",
        "//test/test_util:tcp_client",
        "//test/test_util:temp_dir",
        "//test/test_util:worker_quiescence",
    ],
)

gtest_test(
    name = "host_aliases_test",
    deps = [
        "//src/worker:host_aliases",
    ],
)

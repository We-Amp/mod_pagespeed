# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "cdp_client_test",
    size = "medium",
    deps = [
        "//src/browser:cdp_client",
        "//test/test_util:cdp_pipe",
        "@com_google_absl//absl/status",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "template_detector_test",
    deps = [
        "//src/browser:template_detector",
        "//src/worker:html_scanner",
    ],
)

gtest_test(
    name = "optimization_profile_test",
    deps = [
        "//lib/classify:capability_mask",
        "//src/browser:optimization_profile",
        "@com_google_absl//absl/status",
    ],
)

gtest_test(
    name = "browser_css_extractor_test",
    size = "medium",
    deps = [
        "//src/browser:browser_css_extractor",
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/worker:critical_css_extractor",
        "//test/test_util:cdp_pipe",
        "//test/test_util:cdp_setup_order",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "page_analysis_test",
    size = "medium",
    deps = [
        "//lib/net:fetch_policy",
        "//src/browser:agent_fetcher",
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/browser:page_analysis",
        "//src/browser:render_support",
        "//test/test_util:cdp_pipe",
        "//test/test_util:cdp_setup_order",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "agent_fetcher_test",
    deps = [
        "//lib/net:fetch_policy",
        "//lib/net:upstream_pin",
        "//src/browser:agent_fetcher",
        "//test/test_util:temp_dir",
        "@com_google_absl//absl/strings",
    ],
)

gtest_test(
    name = "analysis_queue_test",
    deps = [
        "//src/browser:analysis_queue",
    ],
)

gtest_test(
    name = "unused_css_remover_test",
    deps = [
        "//src/browser:unused_css_remover",
    ],
)

gtest_test(
    name = "visual_regression_gate_test",
    size = "medium",
    deps = [
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/browser:visual_regression_gate",
        "//test/test_util:cdp_scripted_peer",
        "//test/test_util:cdp_setup_order",
        "//test/test_util:png_image",
        "@com_google_absl//absl/status:statusor",
        "@libpng",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "critical_css_placement_parity_test",
    # Vendored lib/html headers name unused parameters (canonical idiom).
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    deps = [
        "//lib/base:string_util",
        "//lib/base:string_writer",
        "//lib/html:compat",
        "//lib/html:empty_html_filter",
        "//lib/html:html_keywords",
        "//lib/html:html_name",
        "//lib/html:html_node",
        "//lib/html:html_parse",
        "//lib/html:html_writer_filter",
        "//src/browser:critical_css_validator",
        "//src/worker:cascade_layer_order",
        "//src/worker:html_scanner",
        "//src/worker:html_transform_filter",
        "//src/worker:layer_order_sources",
    ],
)

gtest_test(
    name = "critical_css_validator_test",
    size = "medium",
    deps = [
        "//src/browser:cdp_client",
        "//src/browser:critical_css_validator",
        "//src/browser:visual_regression_gate",
        "//src/worker:cascade_layer_order",
        "//src/worker:html_scanner",
        "//test/test_util:cdp_scripted_peer",
        "//test/test_util:png_image",
        "@com_google_absl//absl/strings",
        "@libpng",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "font_glyph_scanner_test",
    size = "medium",
    deps = [
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/browser:font_glyph_scanner",
        "//src/browser:render_support",
        "//test/test_util:cdp_pipe",
        "//test/test_util:cdp_setup_order",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "script_coverage_analyzer_test",
    size = "medium",
    deps = [
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/browser:page_analysis",
        "//src/browser:script_coverage_analyzer",
        "//test/test_util:cdp_pipe",
        "//test/test_util:cdp_setup_order",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "browser_sandbox_test",
    deps = [
        "//src/browser:browser_sandbox",
    ],
)

gtest_test(
    name = "chrome_process_test",
    deps = [
        "//src/browser:chrome_process",
        "@libuv",
    ],
)

gtest_test(
    name = "cdp_utils_test",
    deps = [
        "//src/browser:cdp_utils",
    ],
)

gtest_test(
    name = "browser_analysis_manager_test",
    deps = [
        "//lib/base:message_handler",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:capability_mask",
        "//lib/classify:url_normalizer",
        "//src/browser:browser_css_extractor",
        "//src/browser:critical_css_validator",
        "//src/browser:optimization_profile",
        "//src/browser:page_analysis",
        "//src/browser:script_coverage_analyzer",
        "//src/worker:analysis_resource_map",
        "//src/worker:browser_analysis_manager",
        "//src/worker:css_cache_inliner",
        "//src/worker:html_scanner",
        "//test/test_util:temp_dir",
        "@libuv",
    ],
)

gtest_test(
    name = "device_emulation_test",
    data = ["//src/browser:device_emulation.json"],
    deps = [
        "//lib/classify:capability_mask",
        "//src/browser:cdp_types",
        "//src/browser:device_emulation",
        "@com_google_absl//absl/status",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

gtest_test(
    name = "render_support_test",
    deps = [
        "//src/browser:cdp_client",
        "//src/browser:cdp_types",
        "//src/browser:render_support",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

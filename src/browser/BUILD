# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Browser Module - CDP client and Chrome process management

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "cdp_types",
    hdrs = ["cdp_types.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "browser_sandbox",
    srcs = ["browser_sandbox.cc"],
    hdrs = ["browser_sandbox.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "cdp_client",
    srcs = ["cdp_client.cc"],
    hdrs = ["cdp_client.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_types",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "template_detector",
    srcs = ["template_detector.cc"],
    hdrs = ["template_detector.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//src/worker:html_scanner",
        "@com_google_absl//absl/container:flat_hash_map",
    ],
)

cc_library(
    name = "optimization_profile",
    srcs = ["optimization_profile.cc"],
    hdrs = ["optimization_profile.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/classify:capability_mask",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@cyclone",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "browser_css_extractor",
    srcs = ["browser_css_extractor.cc"],
    hdrs = ["browser_css_extractor.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":cdp_types",
        ":device_emulation",
        ":render_support",
        "//src/worker:cascade_layer_order",
        "//src/worker:critical_css_extractor",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)

# Device emulation for every analysis render. The JSON is the
# one source the async-css probe reads too; device_emulation_test keeps the
# header's constants equal to it.
exports_files(["device_emulation.json"])

cc_library(
    name = "device_emulation",
    srcs = ["device_emulation.cc"],
    hdrs = ["device_emulation.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_types",
        "//lib/classify:capability_mask",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

# Shared by the browser-analysis renders: the one
# Fetch.failRequest, with its reply checked, and the settle timer.
cc_library(
    name = "render_support",
    srcs = ["render_support.cc"],
    hdrs = ["render_support.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":cdp_types",
        "@com_google_absl//absl/strings",
        "@libuv",
    ],
)

cc_library(
    name = "cdp_utils",
    srcs = ["cdp_utils.cc"],
    hdrs = ["cdp_utils.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "page_analysis",
    srcs = ["page_analysis.cc"],
    hdrs = ["page_analysis.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":agent_fetcher",
        ":cdp_client",
        ":cdp_types",
        ":cdp_utils",
        ":device_emulation",
        ":render_support",
        "//lib/html:markdown_extractor_filter",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "agent_fetcher",
    srcs = ["agent_fetcher.cc"],
    hdrs = ["agent_fetcher.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/cache:vary_storability",
        "//lib/net:curl_fetcher",
        "//lib/net:fetch_policy",
        "//lib/net:ssrf_guard",
        "//lib/net:upstream_pin",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "analysis_queue",
    srcs = ["analysis_queue.cc"],
    hdrs = ["analysis_queue.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/container:flat_hash_set",
    ],
)

cc_library(
    name = "unused_css_remover",
    srcs = ["unused_css_remover.cc"],
    hdrs = ["unused_css_remover.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "visual_regression_gate",
    srcs = ["visual_regression_gate.cc"],
    hdrs = ["visual_regression_gate.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":cdp_types",
        ":device_emulation",
        ":render_support",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@libpng",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "critical_css_validator",
    srcs = ["critical_css_validator.cc"],
    hdrs = ["critical_css_validator.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":visual_regression_gate",
        "//lib/base:string_util",
        "//src/worker:cascade_layer_order",
        "//src/worker:html_css_injector",
        "//src/worker:html_scanner",
        "//src/worker:noscript_strip",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "font_glyph_scanner",
    srcs = ["font_glyph_scanner.cc"],
    hdrs = ["font_glyph_scanner.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":cdp_types",
        ":device_emulation",
        ":render_support",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "script_coverage_analyzer",
    srcs = ["script_coverage_analyzer.cc"],
    hdrs = ["script_coverage_analyzer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":cdp_types",
        ":cdp_utils",
        ":device_emulation",
        ":page_analysis",
        ":render_support",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@libuv",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "chrome_process",
    srcs = ["chrome_process.cc"],
    hdrs = ["chrome_process.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":cdp_client",
        ":cdp_types",
        "//src/worker:uv_helpers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@libuv",
    ],
)

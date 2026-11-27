# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary")

# Q3 threshold measurement. Manual: it needs a real browser and the byte-frozen
# fixture, so it is never part of a test run. See the file header for how to
# run it and what its output is for.
cc_binary(
    name = "measure_validation_threshold",
    srcs = ["measure_validation_threshold.cc"],
    tags = ["manual"],
    deps = [
        "//src/browser:browser_css_extractor",
        "//src/browser:chrome_process",
        "//src/browser:critical_css_validator",
        "//src/browser:visual_regression_gate",
        "//src/worker:cascade_layer_order",
        "//src/worker:css_cache_inliner",
        "//src/worker:html_scanner",
        "//src/worker:layer_order_sources",
        "@com_google_absl//absl/status:statusor",
        "@libuv",
    ],
)

# Prints the heuristic critical block the worker would inline for a fixture.
# Manual: a diagnostic for reading a rendered-probe finding, not a test.
cc_binary(
    name = "dump_critical_css",
    srcs = ["dump_critical_css.cc"],
    tags = ["manual"],
    deps = [
        "//lib/classify:capability_mask",
        "//src/worker:cascade_layer_order",
        "//src/worker:critical_css_extractor",
        "//src/worker:html_scanner",
        "//src/worker:layer_order_sources",
        "@com_google_absl//absl/strings",
    ],
)

# Strips <noscript> from every page in a directory, the way the coverage and
# validation renders do. Manual: noscript_compare.mjs drives it
# to check the removal against a scripting Chromium.
cc_binary(
    name = "strip_noscript",
    srcs = ["strip_noscript.cc"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-error=unused-parameter"],
    }),
    tags = ["manual"],
    deps = [
        "//src/worker:html_scanner",
        "//src/worker:noscript_strip",
    ],
)

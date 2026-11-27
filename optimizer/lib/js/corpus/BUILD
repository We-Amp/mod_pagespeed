# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Vendored JS-minifier differential corpus: the probe binary,
# the deterministic corpus generators, and the shared goldens manifest are
# synced byte-aligned from mod_pagespeed 1.15 (see ../JS_KERNEL_PIN and
# ../CLAUDE.md). The manifest is the shared expectation gating BOTH repos:
# CI builds the probe and runs `gen_goldens.py --check` against it.

load("@rules_cc//cc:defs.bzl", "cc_binary")

# Single-file front-end exposing the raw minifier library contract
# (exit 0 = minified, 1 = declined/unmodelable, 2 = usage; crashes propagate
# as signals). Driven by gen_goldens.py; not a test target itself.
cc_binary(
    name = "js_minify_probe",
    srcs = ["js_minify_probe.cc"],
    visibility = ["//:internal"],
    deps = [
        "//lib/js:compat",
        "//lib/js:js_minify",
        "//lib/js:js_tokenizer",
    ],
)

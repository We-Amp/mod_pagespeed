# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Root BUILD file for mod_pagespeed

load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

exports_files([
    "GIT_COMMIT",
    "VERSION.txt",
])

# compile_commands.json generation for clang-tidy / clangd. Wraps hedron's
# default refresh_all (@hedron_compile_commands//:refresh_all is
# refresh_compile_commands with no targets, i.e. {"@//...": ""}) because
# targets tagged `manual` — the fuzz harnesses — are excluded from the
# `//...` wildcard in `bazel aquery`, so their TUs never reach
# compile_commands.json. The lint lane's scoped mode then selects 0 TUs for
# a PR touching only such a file and trips the #1094 fail-closed guard.
# Explicit labels are NOT affected by the manual-tag wildcard exclusion, so
# listing them here puts their compile actions back in the database.
# Maintenance: add future `manual`-tagged cc targets to this dict.
refresh_compile_commands(
    name = "refresh_all",
    targets = {
        "@//...": "",
        "//lib/cache:html_cache_policy_fuzz": "",
        "//lib/css:css_minify_fuzz": "",
        "//lib/html:html_fuzz": "",
    },
)

package_group(
    name = "internal",
    packages = [
        "//lib/...",
        "//src/...",
        "//test/...",
        "//tools/...",
    ],
)

# Platform + architecture config settings for SIMD flag selection.
# Needed because GCC uses -mavx2 while MSVC uses /arch:AVX2,
# and Bazel doesn't allow nested select().

config_setting(
    name = "windows_x86_64",
    constraint_values = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "linux_x86_64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "macos_x86_64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:x86_64",
    ],
    visibility = ["//visibility:public"],
)

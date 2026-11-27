# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Network-trust primitives for the agent_optimize out-of-Chrome fetcher
# (the network-trust kernel). The SSRF guard is a dependency-free,
# hermetically-testable C++ port of agent-readability-scanner/src/ssrf.mjs.

load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "ssrf_guard",
    srcs = ["ssrf_guard.cc"],
    hdrs = ["ssrf_guard.h"],
    visibility = ["//:internal"],
)

cc_library(
    name = "upstream_pin",
    srcs = ["upstream_pin.cc"],
    hdrs = ["upstream_pin.h"],
    visibility = ["//:internal"],
    deps = [":ssrf_guard"],
)

cc_library(
    name = "fetch_policy",
    srcs = ["fetch_policy.cc"],
    hdrs = ["fetch_policy.h"],
    visibility = ["//:internal"],
    deps = [
        ":ssrf_guard",
        ":upstream_pin",
    ],
)

cc_library(
    name = "curl_fetcher",
    srcs = ["curl_fetcher.cc"],
    hdrs = ["curl_fetcher.h"],
    visibility = ["//:internal"],
)

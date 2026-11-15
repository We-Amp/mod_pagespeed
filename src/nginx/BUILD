# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# RFC 9111 §3.5 Authorization shared-cache gate - pure C++, testable
# without nginx headers.
cc_library(
    name = "authz_cache_gate",
    srcs = ["authz_cache_gate.cc"],
    hdrs = ["authz_cache_gate.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/cache:cache_control_header",
        "//lib/classify:alternate_metadata",
    ],
)

# Shared-cache gate for cookie-bearing requests - pure C++, testable
# without nginx headers.  The rule itself is the HTML cache policy's.
cc_library(
    name = "cookie_cache_gate",
    srcs = ["cookie_cache_gate.cc"],
    hdrs = ["cookie_cache_gate.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/cache:html_cache_policy",
        "//lib/classify:content_type",
    ],
)

# 304-restamp storability re-evaluation (issue #1016) - pure C++, testable
# without nginx headers.
cc_library(
    name = "revalidation_storability",
    srcs = ["revalidation_storability.cc"],
    hdrs = ["revalidation_storability.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":authz_cache_gate",
        "//lib/cache:html_cache_policy",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
    ],
)

# Store-side half of the response-header sidecar (issue #208):
# the record arm's classification, sidecar-first record, re-record and 304
# merge decisions - pure C++, testable without nginx headers.
cc_library(
    name = "headers_sidecar_record",
    srcs = ["headers_sidecar_record.cc"],
    hdrs = ["headers_sidecar_record.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//lib/cache:headers_sidecar",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
    ],
)

# Serve-side half of the response-header sidecar (issue #208): which hits
# read the stored header block, the replay plan and its precedence rules, the
# legacy-entry migration rule - pure C++, testable without nginx headers.
cc_library(
    name = "headers_sidecar_replay",
    srcs = ["headers_sidecar_replay.cc"],
    hdrs = ["headers_sidecar_replay.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":headers_sidecar_record",
        "//lib/cache:headers_sidecar",
        "//lib/classify:alternate_metadata",
    ],
)

# Cache-directory generation check against the optimizer's published
# generation - pure C++, testable without nginx headers.  Takes the generation
# from the optimizer's own constant (kCacheDirGeneration) rather than a copy.
cc_library(
    name = "cache_generation_check",
    srcs = ["cache_generation_check.cc"],
    hdrs = ["cache_generation_check.h"],
    visibility = ["//visibility:public"],
    deps = ["//src/worker:shared_config"],
)

# Values of the headers sent with a Markdown response for agents
# (x-markdown-tokens, content-signal) - pure C++, testable without nginx
# headers.
cc_library(
    name = "agent_markdown_headers",
    srcs = ["agent_markdown_headers.cc"],
    hdrs = ["agent_markdown_headers.h"],
    visibility = ["//visibility:public"],
)

# Which request hostnames a server block stands behind (exact and wildcard
# server names, the operator host alias table) - pure C++, testable without
# nginx headers.
cc_library(
    name = "host_vouch",
    srcs = ["host_vouch.cc"],
    hdrs = ["host_vouch.h"],
    visibility = ["//visibility:public"],
)

# The cache keyspace (site) of a server block, from its server names and
# listening addresses - pure C++, testable without nginx headers.  Cyclone is
# here only for its bundled SHA-256.
cc_library(
    name = "site_identity",
    srcs = ["site_identity.cc"],
    hdrs = ["site_identity.h"],
    visibility = ["//visibility:public"],
    deps = ["@cyclone"],
)

# Recognition of the worker's agent fetches arriving from this server (its
# User-Agent marker and a peer that is loopback or the arrival address), which
# the module presents to access rules as 224.0.0.0 - pure C++ over raw address
# bytes, testable without nginx headers.
cc_library(
    name = "agent_self_fetch",
    srcs = ["agent_self_fetch.cc"],
    hdrs = ["agent_self_fetch.h"],
    visibility = ["//visibility:public"],
)

# Early Hints utilities - pure C++, testable without nginx headers.
cc_library(
    name = "early_hints_util",
    srcs = ["early_hints_util.cc"],
    hdrs = ["early_hints_util.h"],
    visibility = ["//visibility:public"],
    deps = ["//lib/base:string_util"],
)

# Lowercase key and hash for request headers the module adds - pure C++.
cc_library(
    name = "header_key_util",
    srcs = ["header_key_util.cc"],
    hdrs = ["header_key_util.h"],
    visibility = ["//visibility:public"],
)

# Weak-ETag construction for cache-HIT responses - pure C++, testable
# without nginx headers.
cc_library(
    name = "etag_util",
    srcs = ["etag_util.cc"],
    hdrs = ["etag_util.h"],
    visibility = ["//visibility:public"],
    deps = ["//lib/classify:alternate_metadata"],
)

# MIME type detection from URL extensions - pure C++.
cc_library(
    name = "mime_util",
    srcs = ["mime_util.cc"],
    hdrs = ["mime_util.h"],
    visibility = ["//visibility:public"],
)

# Zero-copy serve barrier decision - pure C++ (no nginx headers), so
# the keep/copy/abort logic is unit-tested independently of the module.
cc_library(
    name = "zerocopy_barrier",
    hdrs = ["zerocopy_barrier.h"],
    visibility = ["//visibility:public"],
    deps = ["//lib/cache:pagespeed_cache"],
)

# Testable library target: the C++ APIs used by the nginx module.
# This target is used for testing without requiring nginx headers.
cc_library(
    name = "pagespeed_module_deps",
    testonly = True,
    visibility = ["//test:__subpackages__"],
    deps = [
        ":authz_cache_gate",
        ":early_hints_util",
        ":etag_util",
        ":mime_util",
        ":revalidation_storability",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
    ],
)

# Early Hints - nginx-specific implementation (requires nginx headers).
# Tagged "manual" so `bazel build //...` skips it when nginx isn't available.
cc_library(
    name = "early_hints",
    srcs = ["early_hints.cc"],
    hdrs = ["early_hints.h"],
    copts = [
        "-Wno-unused-parameter",
        "-Wno-old-style-cast",
    ],
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [
        ":early_hints_util",
        "@nginx",
    ],
)

# Full module library (requires nginx headers).
# Tagged "manual" so `bazel build //...` skips it when nginx isn't available.
# alwayslink ensures the module's extern "C" symbols survive linker GC
# when building the shared .so (no internal consumers reference them).
cc_library(
    name = "pagespeed_module",
    srcs = ["ngx_pagespeed_module.cc"],
    copts = [
        "-Wno-unused-parameter",
        "-Wno-old-style-cast",
        "-fvisibility=default",
    ],
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [
        ":agent_markdown_headers",
        ":agent_self_fetch",
        ":authz_cache_gate",
        ":cache_generation_check",
        ":cookie_cache_gate",
        ":early_hints",
        ":early_hints_util",
        ":etag_util",
        ":header_key_util",
        ":headers_sidecar_record",
        ":headers_sidecar_replay",
        ":host_vouch",
        ":mime_util",
        ":revalidation_storability",
        ":site_identity",
        ":zerocopy_barrier",
        "//lib/cache:cache_control_header",
        "//lib/cache:freshness",
        "//lib/cache:origin_cache_control",
        "//lib/cache:pagespeed_cache",
        "//lib/cache:vary_emission",
        "//lib/cache:vary_storability",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//lib/classify:url_normalizer",
        "//src/crypto/webbotauth:webbotauth_core",
        "//src/product_version:version",
        "//src/proto:notification_sender",
        "//src/proto:worker_ipc",
        "//src/worker:async_css_loader",
        "//src/worker:serve_stats",
        "@nginx",
    ],
    alwayslink = True,
)

# Dynamic module (.so) for nginx load_module directive.
# Tagged "manual" - must be built explicitly:
#   NGINX_PATH=/path/to/nginx bazel build //src/nginx:ngx_pagespeed_module.so
cc_binary(
    name = "ngx_pagespeed_module.so",
    copts = ["-fvisibility=default"],
    linkopts = select({
        "@platforms//os:macos": ["-Wl,-undefined,dynamic_lookup"],
        "//conditions:default": ["-Wl,--export-dynamic"],
    }),
    linkshared = True,
    linkstatic = True,
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [":pagespeed_module"],
)

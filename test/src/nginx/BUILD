# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

load("//bazel:test.bzl", "gtest_test")

gtest_test(
    name = "ngx_pagespeed_test",
    # Opens several on-disk caches; on hosted Windows runners single opens
    # stall for up to ~20 s, so the whole binary can pass 60 s ("small").
    size = "medium",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//lib/classify:capability_mask",
        "//src/nginx:mime_util",
        "//src/nginx:zerocopy_barrier",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "authz_cache_gate_test",
    deps = [
        "//lib/classify:alternate_metadata",
        "//src/nginx:authz_cache_gate",
    ],
)

gtest_test(
    name = "cookie_cache_gate_test",
    deps = [
        "//lib/cache:html_cache_policy",
        "//lib/classify:alternate_metadata",
        "//lib/classify:content_type",
        "//src/nginx:cookie_cache_gate",
    ],
)

gtest_test(
    name = "agent_self_fetch_test",
    deps = [
        "//lib/net:curl_fetcher",
        "//src/nginx:agent_self_fetch",
    ],
)

gtest_test(
    name = "host_vouch_test",
    deps = ["//src/nginx:host_vouch"],
)

gtest_test(
    name = "site_identity_test",
    deps = [
        "//src/nginx:site_identity",
        "//src/proto:worker_ipc",
    ],
)

gtest_test(
    name = "cache_generation_check_test",
    deps = [
        "//src/nginx:cache_generation_check",
        "//src/worker:shared_config",
    ],
)

gtest_test(
    name = "revalidation_storability_test",
    deps = [
        "//lib/cache:html_cache_policy",
        "//lib/classify:alternate_metadata",
        "//src/nginx:revalidation_storability",
    ],
)

# Cache-effect tests for the restamp dispatch: instantiates the
# ApplyRestampVerdict template against a REAL PageSpeedCache on a temp
# volume, so eviction/skip are asserted by their effect on the cache.
gtest_test(
    name = "restamp_apply_test",
    deps = [
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//src/nginx:revalidation_storability",
        "//test/test_util:temp_dir",
    ],
)

# Store-side response-header sidecar decisions (issue #208): the
# gate on origin header sets, the sidecar-first record and re-record rules
# against a REAL cache on a temp volume, and the 304 merge plan.
gtest_test(
    name = "headers_sidecar_record_test",
    deps = [
        "//lib/cache:headers_sidecar",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//src/nginx:headers_sidecar_record",
        "//test/test_util:temp_dir",
    ],
)

# Serve-side response-header sidecar decisions (issue #208): which hits
# read the stored block, replay order and Vary precedence, missing block ->
# miss, the legacy-entry migration rule, and a record -> serve round trip
# against a REAL cache on a temp volume.
gtest_test(
    name = "headers_sidecar_replay_test",
    deps = [
        "//lib/cache:headers_sidecar",
        "//lib/cache:pagespeed_cache",
        "//lib/classify:alternate_id",
        "//lib/classify:alternate_metadata",
        "//src/nginx:headers_sidecar_record",
        "//src/nginx:headers_sidecar_replay",
        "//test/test_util:temp_dir",
    ],
)

gtest_test(
    name = "etag_util_test",
    deps = [
        "//lib/classify:alternate_metadata",
        "//src/nginx:etag_util",
    ],
)

gtest_test(
    name = "early_hints_test",
    deps = [
        "//src/nginx:early_hints_util",
    ],
)

gtest_test(
    name = "header_key_util_test",
    deps = [
        "//src/nginx:header_key_util",
    ],
)

gtest_test(
    name = "mime_util_test",
    deps = [
        "//src/nginx:mime_util",
    ],
)

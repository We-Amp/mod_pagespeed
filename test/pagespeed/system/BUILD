load("@rules_cc//cc:defs.bzl", "cc_binary")
load(
    "//bazel:pagespeed_test.bzl",
    "pagespeed_cc_test",
    "pagespeed_cc_test_library",
)

licenses(["notice"])  # Apache 2

cc_binary(
    name = "redis_cache_cluster_setup",
    testonly = True,
    srcs = [
        "redis_cache_cluster_setup_main.cc",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base",
    ],
)

# APR-free test base for system tests that don't need APR TCP helpers.
# Use this for tests that only need pagespeed/system and kernel test utilities.
pagespeed_cc_test_library(
    name = "test_base_core",
    deps = [
        "//test/net/instaweb/rewriter:test_base",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/kernel/cache:cache_test_base",
        "//test/pagespeed/kernel/thread:thread_test_base",
    ] + select({
        "@platforms//os:windows": ["//pagespeed/system:system_windows"],
        "//conditions:default": ["//pagespeed/system"],
    }),
)

# Full test base with APR TCP helpers for Redis/Memcached cluster tests.
# Depends on APR for socket operations (Linux-only).
pagespeed_cc_test_library(
    name = "test_base",
    srcs = [
        "redis_cache_cluster_setup.cc",
        "tcp_connection_for_testing.cc",
        "tcp_server_thread_for_testing.cc",
    ],
    hdrs = [
        "redis_cache_cluster_setup.h",
        "tcp_connection_for_testing.h",
        "tcp_server_thread_for_testing.h",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base_core",
        "//pagespeed/apache:apache_apr",
    ],
)

# POSIX TCP server helper - uses POSIX sockets (Linux-only).
pagespeed_cc_test_library(
    name = "posix_tcp_server_for_testing",
    srcs = [
        "tcp_server_thread_for_testing_posix.cc",
    ],
    hdrs = [
        "tcp_server_thread_for_testing_posix.h",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Hermetic in-process HTTP/HTTPS test server + embedded test certificates.
# Replaces the curl fetcher test's former dependency on public-internet hosts
# (httpbin.org, badssl.com, www.google.com). HTTPS uses BoringSSL server-side,
# the same TLS library libcurl links against in this build. Linux-only (POSIX
# sockets + BoringSSL, matching the test target).
pagespeed_cc_test_library(
    name = "curl_test_server",
    srcs = [
        "curl_test_server.cc",
    ],
    hdrs = [
        "curl_test_certs.h",
        "curl_test_server.h",
    ],
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/util",
        "@boringssl//:ssl",
    ],
)

# Hermetic: serves all endpoints from an in-process server on 127.0.0.1 over
# ephemeral ports, so it passes with the public internet unreachable. No
# "external"/"requires-network" tags -- it is part of the default test run and
# the release gate.
pagespeed_cc_test(
    name = "curl_url_async_fetcher_test",
    srcs = [
        "curl_url_async_fetcher_test.cc",
    ],
    shard_count = 1,
    size = "large",
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/system:curl_fetcher",
        "//pagespeed/kernel/http",
        "//pagespeed/kernel/util",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/system:curl_test_server",
        "//test/pagespeed/system:posix_tcp_server_for_testing",
    ],
)

# =============================================================================
# Fine-grained test targets for faster test iteration
# =============================================================================

# External server spec tests - no external server required
pagespeed_cc_test(
    name = "external_server_spec_test",
    srcs = ["external_server_spec_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/system:external_server_spec",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

# AddHeadersFetcher unit test - pure unit test, no external server required.
# The file was present in the tree but built by no target; wire it so its
# cases actually run as part of the default test pass.
pagespeed_cc_test(
    name = "add_headers_fetcher_test",
    srcs = ["add_headers_fetcher_test.cc"],
    shard_count = 1,
    deps = [
        "//test/net/instaweb/rewriter:test_base",
        "//test/pagespeed/system:test_base_core",
    ],
)

# Redis cache tests only - requires Redis server
pagespeed_cc_test(
    name = "redis_cache_test",
    srcs = ["redis_cache_cluster_test.cc"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base",
    ],
)

# Memcached cache tests only - requires Memcached server.
# Uses memcached_cache.h and posix_timer.h (POSIX-only).
pagespeed_cc_test(
    name = "memcached_cache_test",
    srcs = ["memcached_cache_test.cc"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base_core",
    ],
)

# Single-server RedisCache tests (distinct from the wired cluster test, which
# builds redis_cache_cluster_test.cc).  This file was present but built by no
# target.  Like the cluster test it depends on test_base (TCP helpers + APR)
# and self-skips via PrepareRedisOrSkip() when $REDIS_PORT is unset, so it is
# safe in the default run -- start a server (install/run_program_with_redis.sh)
# and set $REDIS_PORT to actually exercise it.
pagespeed_cc_test(
    name = "redis_cache_single_server_test",
    srcs = ["redis_cache_test.cc"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base",
    ],
)

# System core tests - tests that don't require Redis/Memcached
pagespeed_cc_test(
    name = "system_core_test",
    srcs = [
        "in_place_resource_recorder_test.cc",
        "system_message_handler_test.cc",
    ],
    shard_count = 1,
    deps = [
        "//test/pagespeed/system:test_base_core",
    ],
)

# Loopback route fetcher test - uses POSIX sockets (Linux-only).
pagespeed_cc_test(
    name = "loopback_route_fetcher_test",
    srcs = ["loopback_route_fetcher_test.cc"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base_core",
    ],
)

# System caches integration test - requires both Redis and Memcached.
# Uses mkdtemp and /tmp paths (POSIX-only).
pagespeed_cc_test(
    name = "system_caches_test",
    srcs = ["system_caches_test.cc"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base_core",
    ],
)

# Admin site tests - no external server required.
pagespeed_cc_test(
    name = "admin_site_test",
    srcs = ["admin_site_test.cc"],
    shard_count = 1,
    deps = [
        "//test/pagespeed/system:test_base_core",
    ],
)

# AdminLicenseHandler integration tests.
# Uses the testkey variant so tests can sign AND verify tokens with the
# zero-seed Ed25519 keypair.  Depends on admin_license_handler_testkey
# (NOT test_base_core) to avoid ODR violations with the production
# license_verifier that test_base_core transitively pulls in.
pagespeed_cc_test(
    name = "admin_license_handler_test",
    srcs = ["admin_license_handler_test.cc"],
    copts = ["-DPAGESPEED_SERVER=\\\"test\\\""],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/license_v2:license_file",
        "//pagespeed/kernel/license_v2:license_signer",
        "//pagespeed/kernel/license_v2:license_token",
        "//pagespeed/kernel/license_v2:license_verifier_testkey",
        "//pagespeed/kernel/license_v2:tracking_metadata",
        "//pagespeed/system:admin_license_handler_testkey",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

# the design record over-cap matcher unit tests (PSL-free, pure header — no server
# context needed, so a lightweight standalone target).
pagespeed_cc_test(
    name = "over_cap_match_test",
    srcs = ["over_cap_match_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/system:over_cap_match",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

# =============================================================================
# Backward-compatible aggregate test target
# =============================================================================

pagespeed_cc_test(
    name = "system",
    srcs = [
        "external_server_spec_test.cc",
        "in_place_resource_recorder_test.cc",
        "loopback_route_fetcher_test.cc",
        "memcached_cache_test.cc",
        "redis_cache_cluster_test.cc",
        "system_caches_test.cc",
        "system_message_handler_test.cc",
    ],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//test/pagespeed/system:test_base",
    ],
)

# SystemRewriteOptions unit tests (incl. the StrictAdminAccess opt-in gate,
# shared across all server ports). The file was present in the tree but not
# built by any target; wire it so its cases actually run.
pagespeed_cc_test(
    name = "system_rewrite_options_test",
    srcs = ["system_rewrite_options_test.cc"],
    shard_count = 1,
    deps = [
        "//test/net/instaweb/rewriter:test_base",
        "//test/pagespeed/system:test_base_core",
    ],
)


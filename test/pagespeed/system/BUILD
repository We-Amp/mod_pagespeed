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

# A real, dlopen-able stand-in for the daemon's client library.
#
# LoadDaemonAbi is the one part of the adapter no injected fake can reach --
# every other test replaces the loader wholesale -- so its symbol binding,
# version gate and layout handshake had never executed. Three flavours, because
# a symbol cannot be un-exported at run time and each absence is a different
# answer from the loader: the sized initializer is OPTIONAL (an older daemon
# that lacks it must still load), and the Vary-Accept predicate is REQUIRED (a
# library that lacks it is refused, whatever version it claims).
cc_binary(
    name = "libdaemon_stub.so",
    testonly = True,
    srcs = ["daemon_stub_lib.cc"],
    # No C++ runtime: this object is dlopen()ed and dlclose()d once per test,
    # and a statically linked runtime allocates its one-time state on every
    # load without returning it on unload. See daemon_stub_lib.cc.
    linkopts = ["-nostdlib++"],
    linkshared = True,
    linkstatic = True,
    target_compatible_with = ["@platforms//os:linux"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "libdaemon_stub_unsized.so",
    testonly = True,
    srcs = ["daemon_stub_lib.cc"],
    defines = ["PS_STUB_OMIT_SIZED_INIT"],
    linkopts = ["-nostdlib++"],
    linkshared = True,
    linkstatic = True,
    target_compatible_with = ["@platforms//os:linux"],
    visibility = ["//visibility:public"],
)

# A library that REPORTS the floor version but does not export the entry point
# that version publishes. It is the only thing that can tell a required binding
# from an optional one: against a truthful library both behave identically, so
# without this flavour "the Vary-Accept predicate is in the floor" would be a
# claim in a comment rather than a property under test.
cc_binary(
    name = "libdaemon_stub_no_varies_accept.so",
    testonly = True,
    srcs = ["daemon_stub_lib.cc"],
    defines = ["PS_STUB_OMIT_VARIES_ACCEPT"],
    linkopts = ["-nostdlib++"],
    linkshared = True,
    linkstatic = True,
    target_compatible_with = ["@platforms//os:linux"],
    visibility = ["//visibility:public"],
)

# The pre-H1 daemon: no cache_dir_generation reader. Like the sized
# initializer this entry point is bound OPTIONALLY, so this flavour is what
# proves its absence still loads -- the adapter's legacy-layout tolerance is a
# claim in a comment until a library that genuinely lacks the symbol loads.
cc_binary(
    name = "libdaemon_stub_no_generation.so",
    testonly = True,
    srcs = ["daemon_stub_lib.cc"],
    defines = ["PS_STUB_OMIT_GENERATION"],
    linkopts = ["-nostdlib++"],
    linkshared = True,
    linkstatic = True,
    target_compatible_with = ["@platforms//os:linux"],
    visibility = ["//visibility:public"],
)

# Optimizer-daemon adapter startup: the volume-sizing mirror (match starts,
# divergence refuses), the degrade path when the daemon is absent or
# unreachable, and the RAM-tier floor the adapter opens the shared volume
# with. Linux-only: the reachability case binds a real AF_UNIX socket.
pagespeed_cc_test(
    name = "daemon_adapter_test",
    srcs = ["daemon_adapter_test.cc"],
    data = [
        ":libdaemon_stub.so",
        ":libdaemon_stub_no_generation.so",
        ":libdaemon_stub_no_varies_accept.so",
        ":libdaemon_stub_unsized.so",
    ],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/system:daemon_adapter",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

# The record arm: the gate's rules, driven through the REAL client-library
# binder against the real dlopen-able stand-in, so the peer's validation is
# what answers rather than an in-process fake of it.
pagespeed_cc_test(
    name = "daemon_record_arm_test",
    srcs = ["daemon_record_arm_test.cc"],
    data = [":libdaemon_stub.so"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/system:daemon_record_arm",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/system:test_base_core",
    ],
)

# The daemon-side in-place recorder: the seam between a real ResponseHeaders
# and the record gate's input, the checks that run before the gate, and the
# self-owned lifecycle. Driven through the real client-library loader against
# the real dlopen-able stand-in, like the gate's own suite.
pagespeed_cc_test(
    name = "daemon_ipro_recorder_test",
    srcs = ["daemon_ipro_recorder_test.cc"],
    data = [":libdaemon_stub.so"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/system:daemon_record_arm",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/system:test_base_core",
    ],
)

# The serve arm: which stored entry is handed back for one request, and the
# validator / `Vary` / `Cache-Control` state that goes with it. Driven
# through the real client-library loader against the real dlopen-able
# stand-in, whose selector reproduces the peer's scoring arithmetic with the
# format disqualify playable in BOTH generations (the peer's tie-break is not
# modelled; no case scores a positive tie):
# by default without the format disqualify, because an always-disqualifying
# stand-in would make this arm's own disqualify unreachable and every case
# here green by construction, and with it where a case pins what the current
# peer does (no compatible member, so nothing is selected at all).
pagespeed_cc_test(
    name = "daemon_serve_arm_test",
    srcs = ["daemon_serve_arm_test.cc"],
    data = [":libdaemon_stub.so"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//net/instaweb/rewriter",
        "//pagespeed/system:daemon_serve_arm",
        "//test/pagespeed/kernel/base:kernel_test_util",
        "//test/pagespeed/system:test_base_core",
    ],
)

# The in-place record gate: the disposition mapping, and that the classic
# recorder is not CONSTRUCTED on either daemon substrate.
pagespeed_cc_test(
    name = "ipro_record_gate_test",
    srcs = ["ipro_record_gate_test.cc"],
    shard_count = 1,
    target_compatible_with = ["@platforms//os:linux"],
    deps = [
        "//pagespeed/system:daemon_adapter",
        "//test/pagespeed/system:test_base_core",
    ],
)

# the design record optimization thread-count policy: the effective-CPU-budget
# arithmetic (min of online CPUs / affinity mask / cgroup quota), the cgroup
# file parsing, and the budget formula.  Pure unit test -- no server, no
# container, no dependence on the host's core count.
pagespeed_cc_test(
    name = "optimization_thread_policy_test",
    srcs = ["optimization_thread_policy_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/system:optimization_thread_policy",
        "//test/pagespeed/kernel/base:kernel_test_util",
    ],
)

# SystemRewriteDriverFactory thread-count autodetection tests: what
# NumRewriteThreads / NumExpensiveRewriteThreads resolve to on each port
# shape, and that an explicit setting always wins over the detected default.
pagespeed_cc_test(
    name = "system_rewrite_driver_factory_test",
    srcs = ["system_rewrite_driver_factory_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/system:optimization_thread_policy",
        "//test/net/instaweb/rewriter:test_base",
        "//test/pagespeed/system:test_base_core",
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


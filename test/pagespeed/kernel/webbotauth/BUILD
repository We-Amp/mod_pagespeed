load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test")

licenses(["notice"])

# Web-Bot-Auth (RFC 9421) unit tests -- relocated here from
# //pagespeed/kernel/webbotauth so the CI //test/pagespeed/kernel/... build and
# test globs in CI actually COMPILE and RUN them across
# every job (Linux x64/ARM64, Windows, ASan+UBSan, TSan, system). Before this
# move they were linted (gen_compilation_database //pagespeed/...) but NEVER
# built or run -- no //pagespeed/kernel/webbotauth/... glob exists. The
# cc_library/cc_binary targets stay at //pagespeed/kernel/webbotauth.
#
# pagespeed_cc_test auto-injects //test:main + //test/pagespeed/kernel/base:
# pagespeed_gtest (bazel/pagespeed_test.bzl:54-56), so NO @googletest//:gtest_main
# dep. Cross-package self-refs (:webbotauth_core) become absolute labels.
# All six test sources are Apache-2.0 -- the per-file license headers stand on
# their own, package-level licenses(["notice"]) mirrors the proofwire reference.

# Hermetic, offline. Deterministic Ed25519 keypair (fixed seed), fixed now_ clock
# passed explicitly, FakeKeyDirectoryProvider. No network/threads/filesystem.
pagespeed_cc_test(
    name = "verifier_test",
    size = "small",
    srcs = ["verifier_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/webbotauth:webbotauth_core",
        "@ed25519",
    ],
)

# Opt-in Web-Bot-Auth counter store: FNV-1a-64 keyid hash (asserted cross-engine
# compatible with the 2.0 optimizer line), lock-free slot claim + overflow (single-threaded and
# under 8-thread contention), latency bucketing, boot-identity minting, and the
# mmap open/create/self-heal round-trip. Uses a temp file; no network.
pagespeed_cc_test(
    name = "webbotauth_counter_store_test",
    size = "small",
    srcs = ["webbotauth_counter_store_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/webbotauth:webbotauth_counter",
    ],
)

# Pure in-memory JWKS string parsing. No @ed25519 needed (uses the 43-'A'
# base64url = 32 zero bytes trick). No network/threads/filesystem/clock.
pagespeed_cc_test(
    name = "static_key_directory_test",
    size = "small",
    srcs = ["static_key_directory_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/webbotauth:webbotauth_core",
    ],
)

# Hermetic, mirrors verifier_test. Deterministic salted seed, fixed kNow,
# FakeKeyDirectoryProvider / OffAllowlistProvider (asserts NO fetch). Pure CPU.
pagespeed_cc_test(
    name = "rsl_cap_validator_test",
    size = "small",
    srcs = ["rsl_cap_validator_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/webbotauth:webbotauth_core",
        "@ed25519",
    ],
)

# Real in-memory single-threaded blocking LRUCache (no TSan surface) + CountingProvider
# trap fakes; explicit unix-second clocks. No @ed25519. Needs cache for lru_cache.h.
pagespeed_cc_test(
    name = "key_directory_test",
    size = "small",
    srcs = ["key_directory_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache:cache",
        "//pagespeed/kernel/webbotauth:webbotauth_core",
    ],
)

# SSRF-guard test: fetcher is nullptr on every path; only the fail-closed GetKey
# branches + pure IP/URL predicates run BEFORE any I/O. No network/threads/DNS.
# Depends on the heavier :webbotauth_netfetch closure (never built in CI before)
# + //pagespeed/opt/http:http (request_context.h).
pagespeed_cc_test(
    name = "net_fetch_key_directory_test",
    size = "small",
    srcs = ["net_fetch_key_directory_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/webbotauth:webbotauth_netfetch",
        "//pagespeed/opt/http:http",
    ],
)

# NEEDS-CARE: the only multi-threaded test. DeferredFetcher spawns a real
# ThreadSystem::Thread that completes the fetch cross-thread while the warmer
# blocks in SyncFetcherAdapterCallback::TimedWait (25ms completer vs 5000ms
# fetch_timeout). First-ever TSan/ARM64 exposure -- GATE on a clean local
# --config=tsan run before merge; if TSan flags the condvar/callback hand-off or
# the 25ms margin flakes under load, add tags = ["no_tsan"] here (do NOT block
# the other five). No real network (in-process fakes), no filesystem.
pagespeed_cc_test(
    name = "key_directory_warmer_test",
    size = "small",
    srcs = ["key_directory_warmer_test.cc"],
    shard_count = 1,
    deps = [
        "//net/instaweb/http:http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache:cache",
        "//pagespeed/kernel/http:http",
        "//pagespeed/kernel/util:util",
        "//pagespeed/kernel/webbotauth:webbotauth_core",
        "//pagespeed/kernel/webbotauth:webbotauth_warmer",
    ],
)

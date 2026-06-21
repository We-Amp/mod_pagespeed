load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

package(default_visibility = ["//visibility:public"])

# Port-agnostic, nginx-headers-free Web-Bot-Auth (RFC 9421) verifier core.
# This is the unit-testable unit: its dependency closure is @ed25519 +
# pagespeed_base + the cache *interface* only -- no nginx, no serf fetcher, no
# Cyclone implementation. The crypto primitive (ed25519_verify) is REUSED from
# @ed25519, the same dep as //pagespeed/kernel/license_v2 (no parallel crypto).
cc_library(
    name = "webbotauth_core",
    srcs = [
        "base64.cc",
        "classifier.cc",
        "header_parser.cc",
        "jwks.cc",
        "key_directory.cc",
        "rsl_cap_token.cc",
        "rsl_cap_validator.cc",
        "signature_base.cc",
        "sfv.cc",
        "static_key_directory.cc",
        "verifier.cc",
    ],
    hdrs = [
        "base64.h",
        "classifier.h",
        "header_parser.h",
        "jwks.h",
        "key_directory.h",
        "rsl_cap_token.h",
        "rsl_cap_validator.h",
        "signature_base.h",
        "sfv.h",
        "static_key_directory.h",
        "verifier.h",
    ],
    deps = [
        "@ed25519",
        # cache_interface.h lives in :pagespeed_base; the verifier core depends
        # only on the CacheInterface ABSTRACTION, never on CycloneCache itself
        # (the nginx wiring pass injects a concrete CycloneCache).
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Production network-fetch key-directory provider (the network-trust model:
# host allowlist + SSRF guard + timeout + size cap). Pulls the engine URL
# fetcher + http stack and is wired into the nginx server context in a later,
# default-off pass. Deliberately SEPARATE from :webbotauth_core so the unit
# test below does NOT need it.
cc_library(
    name = "webbotauth_netfetch",
    srcs = ["net_fetch_key_directory.cc"],
    hdrs = ["net_fetch_key_directory.h"],
    deps = [
        ":webbotauth_core",
        "//net/instaweb/http:http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
        "//pagespeed/opt/http:http",
    ],
)

# the design record A2 background key-directory warmer: a dedicated per-worker thread that
# does a bounded, SSRF-guarded blocking HTTPS fetch of the directory JWKS off the
# request path and populates the shared blocking cache (the request path reads
# cache-only). Pulls the engine fetcher + http stack + threading; SEPARATE from
# :webbotauth_core so the hermetic core tests need none of it. Wired into the
# nginx child-init in a default-off pass.
cc_library(
    name = "webbotauth_warmer",
    srcs = ["key_directory_warmer.cc"],
    hdrs = ["key_directory_warmer.h"],
    deps = [
        ":webbotauth_core",
        ":webbotauth_netfetch",
        "//net/instaweb/http:http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/http:http",
        "//pagespeed/opt/http:http",
    ],
)

# Hermetic, offline unit test. Mints its own Ed25519 keypair with @ed25519
# (ed25519_create_keypair / ed25519_sign), injects a fake key directory, and
# asserts the four success-bar cases plus robustness cases. Depends ONLY on the
# core library -- no network, no nginx, no serf, no Cyclone.
cc_test(
    name = "verifier_test",
    size = "small",
    srcs = ["verifier_test.cc"],
    deps = [
        ":webbotauth_core",
        "@ed25519",
        "@googletest//:gtest_main",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Hermetic, offline unit test for the in-memory local-JWKS provider (A1 v1).
cc_test(
    name = "static_key_directory_test",
    size = "small",
    srcs = ["static_key_directory_test.cc"],
    deps = [
        ":webbotauth_core",
        "@googletest//:gtest_main",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Hermetic, offline unit test for the RSL-CAP capability-token validator (the
# PAID enforcement sibling of the FREE verifier). Mints its own Ed25519
# keypairs, hand-assembles tokens, and injects a fake KeyDirectoryProvider --
# no network, no nginx, no serf, no Cyclone. Depends ONLY on the core library.
cc_test(
    name = "rsl_cap_validator_test",
    size = "small",
    srcs = ["rsl_cap_validator_test.cc"],
    deps = [
        ":webbotauth_core",
        "@ed25519",
        "@googletest//:gtest_main",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

# Hermetic, offline unit tests for the design record A2 cache layer:
# WarmKeyDirectoryCache, CachedKeyDirectoryProvider read-through vs cache-only,
# ChainedKeyDirectoryProvider, and ExtractAllEd25519Keys. Uses a real in-memory
# blocking LRUCache; no network, no nginx, no threads.
cc_test(
    name = "key_directory_test",
    size = "small",
    srcs = ["key_directory_test.cc"],
    deps = [
        ":webbotauth_core",
        "@googletest//:gtest_main",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache:cache",
    ],
)

# Hermetic SSRF-guard test for the network-fetch provider (the trust boundary
# from #211, previously untested). Pure predicates + the fail-closed branches of
# GetKey that return before any fetch -- so no real fetcher is needed.
cc_test(
    name = "net_fetch_key_directory_test",
    size = "small",
    srcs = ["net_fetch_key_directory_test.cc"],
    deps = [
        ":webbotauth_netfetch",
        "@googletest//:gtest_main",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/opt/http:http",
    ],
)

# Test for the A2 warmer's synchronous fetch+populate cycle, driven by an
# inline fake fetcher (the dedicated thread is not started).
cc_test(
    name = "key_directory_warmer_test",
    size = "small",
    srcs = ["key_directory_warmer_test.cc"],
    deps = [
        ":webbotauth_core",
        ":webbotauth_warmer",
        "@googletest//:gtest_main",
        "//net/instaweb/http:http",
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/cache:cache",
        "//pagespeed/kernel/http:http",
        "//pagespeed/kernel/util:util",
    ],
)

# Offline CLI used ONLY by the AgentPass A1 live smoke test: mints a
# deterministic Ed25519 keypair and emits the matching JWKS + RFC 9421
# Signature-Input/Signature headers (built with the library serializer so they
# are bit-identical to what the verifier checks). Not shipped in any runtime.
cc_binary(
    name = "webbotauth_sign_tool",
    srcs = ["webbotauth_sign_tool.cc"],
    deps = [
        ":webbotauth_core",
        "@ed25519",
        "//pagespeed/kernel/base:pagespeed_base",
    ],
)

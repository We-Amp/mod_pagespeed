load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

# Port-agnostic, nginx-headers-free Web-Bot-Auth (RFC 9421) verifier core.
# This is the unit-testable unit: its dependency closure is @ed25519 +
# pagespeed_base + the cache *interface* only -- no nginx, no serf fetcher, no
# Cyclone implementation. The crypto primitive (ed25519_verify) is REUSED from
# @ed25519, the same dep as //pagespeed/kernel/proofwire (no parallel crypto).
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

# Web Bot Auth opt-in counter store:
# a fixed-layout, memory-mapped, cross-process counter file (per-signer slot
# table + coarse verify-latency histogram + a boot identity) that the nginx
# module maps and the /.well-known/webbotauth-counter endpoint reads. Kept
# SEPARATE from :webbotauth_core so the hermetic verifier core stays free of the
# mmap/platform code; its only dep is pagespeed_base.
cc_library(
    name = "webbotauth_counter",
    srcs = ["webbotauth_counter_store.cc"],
    hdrs = ["webbotauth_counter_store.h"],
    deps = [
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

load("@rules_cc//cc:defs.bzl", "cc_library")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

# ProofWire served-evidence ledger -- FREE LOCAL tier.
#
# Append-only, hash-chained, Ed25519-signed flight recorder. Reuses the external
# @ed25519 library for both signing/verification and its bundled SHA-512 (no new
# crypto dependency). DEFAULT-OFF and NON-SHIPPING: a self-contained primitive
# not wired into any request or serving path. Its unit test lives under
# //test/pagespeed/kernel/proofwire so the CI //test/... globs build and run it.
cc_library(
    name = "ledger",
    srcs = ["ledger.cc"],
    hdrs = ["ledger.h"],
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "@ed25519",
    ],
)

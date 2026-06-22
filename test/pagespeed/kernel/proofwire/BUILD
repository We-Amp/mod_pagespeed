load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test")

licenses(["notice"])

# ProofWire ledger unit test -- FREE LOCAL tier.
#
# Lives here under //test/... (the canonical test-tree location) so the CI
# //test/pagespeed/kernel/... build and test globs actually COMPILE and RUN it
# across every job; the library itself stays at //pagespeed/kernel/proofwire.
# pagespeed_cc_test injects //test:main + the shared gtest fixture, so no
# gtest_main dep is needed here. @ed25519 is a direct dep for the
# ed25519_create_keypair fixture (fully offline, no @modpagespeed2 fetch);
# //pagespeed/kernel/base is declared directly because the test #includes
# base/string.h + string_util.h itself.
pagespeed_cc_test(
    name = "ledger_test",
    size = "small",
    srcs = ["ledger_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/proofwire:ledger",
        "@ed25519",
    ],
)

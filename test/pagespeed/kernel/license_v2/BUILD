# Copyright (c) 2024-2026 We-Amp B.V.
#
# license_v2 unit tests -- relocated here from //pagespeed/kernel/license_v2 so
# the CI //test/pagespeed/kernel/... build and test globs actually COMPILE and
# RUN them (before this they were linted but NEVER built/run -- no
# //pagespeed/kernel/license_v2/... glob exists). The cc_library/cc_binary
# targets (license_token, license_verifier[_testkey], license_signer,
# license_file, generate_license_token, tracking_metadata) STAY at
# //pagespeed/kernel/license_v2.
#
# These are hand-written We-Amp tests (NO "DO NOT EDIT BY HAND" banner); they are
# NOT among the six vendored crypto SOURCE files
# (license_token/verifier/signer .{h,cc}) that crypto-drift-check.yml asserts via
# `git ls-files --error-unmatch` + `git diff -- pagespeed/kernel/license_v2/`.
# Moving the tests OUT of that dir only shrinks what the drift diff scans -- it
# is strictly safer and does not perturb the guard.
#
# pagespeed_cc_test auto-injects //test:main + //test/pagespeed/kernel/base:
# pagespeed_gtest, so NO @googletest//:gtest_main dep. No `defines` are needed on
# the test targets: the PAGESPEED_LICENSE_NAMESPACE / PAGESPEED_PRODUCT_ID -D
# flags are compiled into the LIBRARY targets only; the tests write
# `namespace net_instaweb` literally and link the already-defined libraries (the
# source-package cc_test targets carried no defines either).

load("//bazel:pagespeed_test.bzl", "pagespeed_cc_test")

# Pure base64url/JSON serialization round-trips. No network/threads/clock/
# testdata. //pagespeed/kernel/base:pagespeed_base declared directly (the .cc
# #includes base/string.h + string_util.h).
pagespeed_cc_test(
    name = "license_token_test",
    size = "small",
    srcs = ["license_token_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/license_v2:license_token",
    ],
)

# NEEDS-CARE: in-process Ed25519 (zero-seed keypair, sign+verify in memory) +
# calls ed25519_sign directly -> keep @ed25519. Links the PRODUCTION
# :license_verifier (embedded-key paths), NOT :license_verifier_testkey -- it
# only asserts non-embedded keys FAIL, so NO testkey genrule/fixture is needed.
# No network/threads/filesystem; ed25519 already runs in CI for proofwire +
# webbotauth so ASan/UBSan/TSan are fine.
pagespeed_cc_test(
    name = "license_verifier_test",
    size = "small",
    srcs = ["license_verifier_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/license_v2:license_signer",
        "//pagespeed/kernel/license_v2:license_token",
        "//pagespeed/kernel/license_v2:license_verifier",
        "@ed25519",
    ],
)

# NEEDS-CARE: filesystem-touching. Writes to temp_directory_path() with a
# pid+atomic-counter-unique path (collision-free across shards/ASan reruns);
# TearDown restores perms + removes. The chmod(000) EACCES case is GTEST_SKIP'd
# when geteuid()==0 and #ifdef-skipped on _WIN32; CI Linux containers run
# non-root --user so it RUNS and must genuinely get EACCES. shard_count=1
# (shared temp dir + perms manipulation), mirroring proofwire's ledger_test.
pagespeed_cc_test(
    name = "license_file_test",
    size = "small",
    srcs = ["license_file_test.cc"],
    shard_count = 1,
    deps = [
        "//pagespeed/kernel/base:pagespeed_base",
        "//pagespeed/kernel/license_v2:license_file",
    ],
)

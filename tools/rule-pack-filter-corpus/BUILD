licenses(["notice"])  # Apache 2

# Seed inputs for the rule-pack filter fuzz harness: every fixture page plus a
# few malformed ones, each behind the two header bytes the harness reads.
# Replayed by //test/pagespeed/kernel/packs:rule_pack_filter_fuzz_replay_test
# and used to seed the nightly coverage-guided run.
filegroup(
    name = "seeds",
    srcs = glob(["seeds/*"]),
    visibility = ["//visibility:public"],
)

licenses(["notice"])  # Apache 2

# Seed inputs for the pack loader fuzz harness: valid packs and one file per
# distinct loader error input, plus the example pack from the test data (not
# copied). Replayed by //test/pagespeed/kernel/packs:pack_loader_fuzz_replay_test
# and used to seed the nightly coverage-guided run.
filegroup(
    name = "seeds",
    srcs = glob(["seeds/*"]) + [
        "//test/pagespeed/kernel/packs:testdata/edge-seo/pack.example.json",
    ],
    visibility = ["//visibility:public"],
)

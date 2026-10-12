# Pack loader fuzz corpus

Seed inputs for `//pagespeed/kernel/packs:pack_loader_fuzz`, the memory-safety
harness for `LoadPack()`.

- `seeds/valid-*.json` -- a pack that exercises every rule kind. The example
  pack (`test/pagespeed/kernel/packs/testdata/edge-seo/pack.example.json`) is
  also a seed, referenced rather than copied.
- `seeds/error-*.json` -- inputs the loader rejects, one per distinct failing
  input in the loader's unit tests (`test/pagespeed/kernel/packs/pack_loader_test.cc`):
  unknown and duplicate keys, wrong types, bad globs and regular expressions,
  limits, malformed and over-nested JSON, invalid UTF-8.

Replay (deterministic, no sanitizer needed):

    bazel test //test/pagespeed/kernel/packs:pack_loader_fuzz_replay_test

Replay under AddressSanitizer:

    bazel run --config=clang-asan -c opt //pagespeed/kernel/packs:pack_loader_fuzz -- \
        tools/pack-loader-corpus/seeds/*

Coverage-guided run (the nightly fuzz workflow does this with a time budget):
build with `--copt=-DPACK_LOADER_LIBFUZZER --copt=-fsanitize=fuzzer-no-link
--linkopt=-fsanitize=fuzzer` and pass a corpus directory seeded from `seeds/`.

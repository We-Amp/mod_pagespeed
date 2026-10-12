# Rule-pack filter fuzz corpus

Seed inputs for `//pagespeed/kernel/packs:rule_pack_filter_fuzz`, the
memory-safety harness for `RulePackFilter`.

Each file is two header bytes followed by an HTML document:

- byte 0 picks the page path, and so which of the harness's fixed rules
  apply (`% 6`: `/k` keep, `/r` repair, `/x` replace, each with its
  canonical, title, description, hreflang and JSON-LD rule); its high bit
  turns on debug comments;
- byte 1 seeds the chunk sizes the document is cut into, with a `Flush()`
  after every chunk.

The harness runs each input enforcing, report-only and without the filter,
and aborts if the report-only output differs from the output without the
filter.

- `seeds/<fixture>.bin` -- the input page of every fixture under
  `test/pagespeed/kernel/packs/testdata/edge-seo/fixtures/`.
- the other seeds -- malformed and adversarial heads: unclosed elements,
  nested heads, many titles, invalid entities and bytes, head kinds only in
  the body.

Replay (deterministic, no sanitizer needed):

    bazel test //test/pagespeed/kernel/packs:rule_pack_filter_fuzz_replay_test

Replay under AddressSanitizer:

    bazel run --config=clang-asan -c opt //pagespeed/kernel/packs:rule_pack_filter_fuzz -- \
        tools/rule-pack-filter-corpus/seeds/*

Coverage-guided run (the nightly fuzz workflow does this with a time budget):
build with `--copt=-DRULE_PACK_FILTER_LIBFUZZER --copt=-fsanitize=fuzzer-no-link
--linkopt=-fsanitize=fuzzer` and pass a corpus directory seeded from `seeds/`.

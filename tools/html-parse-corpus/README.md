# HTML parse corpus

Canonical differential HTML parse harness for mod_pagespeed 1.15 and
ModPageSpeed 2.0. **Read `SPEC.md` first** — it is the contract phase 2
implements against in the optimizer.

Layout:

- `SPEC.md` — parse event-stream format, probe session setup, exit codes,
  residual-divergence policy. Normative.
- `EXCLUSIONS.md` — inputs excluded from cross-product goldens, with
  reasons. Starts empty (see SPEC.md §5).
- `seeds/` — hand-written input battery (checked in). Each seed carries the
  project's SPDX header as a leading HTML comment — placed after the doctype
  line(s) in the doctype battery so the doctype position under test never
  moves — except the three seeds whose test is their exact byte content
  (`empty.html`, `whitespace-only.html`, `text-only.html`), which carry none.
  The goldens record that comment as the first event of every headered seed.
- `gen_extra_inputs.py` — deterministic generator for inputs too large to
  check in (regenerated into a temp dir on every goldens run).
- `gen_goldens.py` — goldens generator / freshness gate.
- `goldens/manifest.json` — one expected result per input, shared by both
  products.

The probe (`//pagespeed/kernel/html:html_parse_probe`) and the fuzz target
(`//pagespeed/kernel/html:html_fuzz`) live in `pagespeed/kernel/html/` next
to the parser, sharing `html_parse_recorder.h`.

## Build

```sh
bazel build --config=macos-asan -c opt //pagespeed/kernel/html:html_parse_probe
```

## Regenerate / verify goldens

```sh
python3 tools/html-parse-corpus/gen_goldens.py          # regenerate
python3 tools/html-parse-corpus/gen_goldens.py --check  # freshness gate (CI)
```

`--check` exits non-zero with a unified diff if parser behavior or any
corpus/generator input changed without regenerating the manifest. Phase 2
wires this into CI alongside the optimizer differential run; no CI workflow is
added in phase 1.

## Fuzzing

Standalone replay over the corpus (ASan is the oracle):

```sh
bazel build --config=macos-asan -c opt //pagespeed/kernel/html:html_fuzz
./bazel-bin/pagespeed/kernel/html/html_fuzz tools/html-parse-corpus/seeds/*
```

Coverage-guided libFuzzer mode (homebrew LLVM clang; the css_parser
`-DCSS_PARSER_LIBFUZZER` precedent):

```sh
CC=/opt/homebrew/opt/llvm/bin/clang bazel build --config=macos-asan -c opt \
  --copt=-DHTML_PARSE_LIBFUZZER --copt=-fsanitize=fuzzer \
  --linkopt=-fsanitize=fuzzer \
  //pagespeed/kernel/html:html_fuzz
mkdir -p /tmp/html-fuzz-corpus && cp tools/html-parse-corpus/seeds/* /tmp/html-fuzz-corpus/
./bazel-bin/pagespeed/kernel/html/html_fuzz /tmp/html-fuzz-corpus -max_total_time=60
```

Any crash is a finding: minimize and report; do not fix the lexer in the
harness branch.

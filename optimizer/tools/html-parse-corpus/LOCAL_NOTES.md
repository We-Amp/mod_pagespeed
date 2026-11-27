# Local notes — vendored HTML parse corpus (local notes)

Canonical source: the `mod_pagespeed` 1.15 repo, same relative path
(`tools/html-parse-corpus/`, phase 1; tracking
**Read `SPEC.md` first** — it is the
contract this repo's probe implements against.

## Layout choice

The corpus is vendored at the *same relative path* it has in the 1.15
repo, so byte-level sync/diff of the shared material is trivial
(`diff -r` against the canonical tree). The D1 JS corpus had to live
under `lib/js/corpus/` because it is pinned by the kernel sync
(`tools/sync-js-kernel.sh`); D2 deliberately has no kernel-vendoring
machinery yet, so the 1.15-path mirror is the lowest-drift option. The
probe and fuzz target mirror their 1.15 placement too: next to the
parser (`pagespeed/kernel/html/` there, `lib/html/` here).

## Byte-aligned with the canonical tree (do not edit here)

- `SPEC.md`, `README.md`, `EXCLUSIONS.md`, `gen_goldens.py`,
  `gen_extra_inputs.py`, `seeds/`, `goldens/manifest.json`

`gen_goldens.py` stays byte-identical (D1 precedent): the shared
manifest pins its sha256 and `gen_extra_inputs.py`'s, so any local edit
would invalidate the pin in both repos. Its default probe path points at
the 1.15 bazel layout; always pass `--probe` here. `EXCLUSIONS.md` is
fully canonical since 2026-07-27 (the lowercased-FPI entry moved to the
1.15 file); keep it byte-aligned.

## Local files (owned here)

- `check_manifest.py` — the CI gate: the local probe's output vs the vendored
  shared manifest, honoring `EXCLUSIONS.md`. Exists because the
  byte-aligned `gen_goldens.py --check` knows no exclusions.
- `LOCAL_NOTES.md` — this file.

## Build / run

```sh
./tools/docker-test.sh build //lib/html:html_parse_probe //lib/html:html_fuzz
PROBE=$(find /tmp -path "*bin/lib/html/html_parse_probe" -type f 2>/dev/null | head -1)
python3 tools/html-parse-corpus/check_manifest.py --probe "$PROBE"   # CI gate
python3 tools/html-parse-corpus/gen_goldens.py --check --probe "$PROBE" \
  # freshness gate; goes red on excluded inputs by design — use
  # check_manifest.py as the local gate until exclusions are canonical
```

The fuzz target replays corpus inputs through the probe path with the
sanitizer as oracle (ASan: `--config=asan`); see `lib/html/html_fuzz.cc`.
On macOS, run it with `ASAN_OPTIONS=detect_leaks=0`: the oracle is heap
errors, on parity with phase 1 (macOS ASan has no LSan). On Linux the
replay is LSan-clean with `ASAN_OPTIONS=detect_leaks=1` — the two leak
shapes formerly flagged here (merged-away characters-node Data, and Data
of elements never event-emitted) were fixed in
the local conformance work.

One caveat on how that evidence was produced: the LSan-clean acceptance
run for #1116 was done with the suppressions file explicitly disabled
(no `LSAN_OPTIONS=suppressions=...`). The default `--config=asan-leaks`
wires `tools/lsan_suppressions.txt`, whose `HtmlLeafNode` /
`HtmlCharactersNode` / `HtmlElement` entries cover exactly the frames
those fixes address — so under the default gate the #1116 regression
tests are leak-vacuous: the suppressions-disabled replay, not the test
suite alone, is the guard. The remaining leak families (filter-driven
`MarkAsDead` paths) and the narrowing of those suppressions are tracked
in the corpus-conformance follow-up.

# JS minifier corpus stress tooling

Spike tooling to stress the tokenizer-based JS minifier
(`pagespeed/kernel/js/js_minify.cc` — `MinifyUtf8Js` via
`JsMinifyingTokenizer`, the `UseExperimentalJsMinifier` path) against
real-world and synthetic JavaScript, with automated oracles. Discovery, not
fixes: failures are minimized and reported, the tokenizer is not modified.

## Layout

- `js_minify_probe.cc` + `BUILD` — single-file probe binary exposing the raw
  minifier contract: exit 0 = minified, exit 1 = "unmodelable" (the
  byte-preserving pass-through case), signals = crashes. Modes: `tokenizer`,
  `legacy`, `legacy-collapse`. Unlike `net/instaweb/rewriter/js_minify_main.cc`
  it does not mask failures with a trimmed-input fallback, so the harness can
  observe exactly what the library returns.
- `manifest.json` — pinned npm packages (name, version, tarball URL, sha256,
  member files) plus a `bundles` section pinning the bundler tool versions
  and the expected sha256 of every locally-built bundle output. Third-party
  sources are fetched, never committed.
- `fetch_corpus.py` — `resolve` (rebuild the pinned manifest from the live
  registry) and `fetch` (verify sha256, extract member files into
  `.corpus/real/`).
- `bundle-fixtures/` — first-party toy entry sources + pinned bundler
  toolchain (`package.json`/`package-lock.json`: webpack, rollup, esbuild).
  Covers the bundled-output shapes the npm dist files do not: webpack
  runtime boilerplate + eval-free module wrappers, bundler-minified output,
  legal-banner comments, `//# sourceMappingURL=` comments, a lazy chunk, and
  a bundled ES module. Only our own sources are committed.
- `build_bundles.py` — `build` (npm ci once, run the three bundlers into
  `.corpus/bundles/`, verify output hashes against the manifest — drift is a
  warning, tool versions are the pin) and `record` (re-record output hashes
  in `manifest.json` after bumping tools or fixture sources). Writes an
  `index.json` with per-bundle goal/family metadata for the oracle run.
- `generate_synthetic.py` — seeded, deterministic generator. Emits parse-only
  shape probes (ASI traps, modern syntax, comment torture, unicode, giant
  flat files, deep nesting) and executable self-checking probes that print
  `RESULT <value>` for semantic comparison. `index.json` records per-file
  module-ness/executability.
- `run_oracles.py` — the harness. Per file: goal classification
  (script/module, with reclassification fallback), input parse check,
  tokenizer + legacy probe runs, output parse check (`node --check`),
  idempotence (minify twice, compare bytes), semantic stdout diff for
  executable probes, legacy-vs-tokenizer divergence. Writes
  `records.jsonl`, `summary.json`, `SUMMARY.md`.
- `minimize.py` — ddmin-style greedy reducer that preserves a given failure
  (parse / semantic / idempotence), bounded by `--max-evals`.
- `run.sh` — end-to-end: build probe, fetch, build bundles, generate,
  oracle run. Extra probe-build bazel flags can be passed via the
  `BAZEL_BUILD_OPTIONS` env var (used by the CI lane).

## Usage

```sh
tools/js-minify-corpus/run.sh                  # full run (needs network once)
tools/js-minify-corpus/run.sh --no-fetch       # synthetic-only / offline
```

Artifacts land in `tools/js-minify-corpus/.corpus/` (gitignored):
`real/`, `bundles/`, `synthetic/`, `reports/<timestamp>/`.

Minimize a finding:

```sh
python3 tools/js-minify-corpus/minimize.py \
  --probe bazel-bin/tools/js-minify-corpus/js_minify_probe \
  --kind parse --goal script input.js repro.js
```

## Oracles (what counts as a bug)

1. Minified output does not parse (`node --check`, correct script/module
   goal).
2. Output parses but changed meaning (executable probes: stdout/exit-code
   diff between original and minified).
3. Non-idempotence: `minify(minify(x)) != minify(x)`.
4. Crashes/asserts (probe killed by a signal).
5. Legacy-vs-tokenizer divergence: exactly one implementation produces
   parsing output for the same input.

"Unmodelable" results (exit 1) are *not* bugs — that is the byte-preserving
pass-through contract — but the pass-through rate is a health metric for
deprecating the legacy minifier.

## Notes

- Python is stdlib-only; node (v24+) and bazel are required. The bundle
  fixtures additionally need npm (network for the first `npm ci` only).
- The synthetic generator is deterministic (`--seed`, default 20260721).
- Bundle builds are deterministic too: fixed output filenames, no content
  hashing, toolchain fully pinned by `bundle-fixtures/package-lock.json`;
  `build_bundles.py build` re-verifies the recorded output sha256s on every
  run and `record` updates them.
- Repros derived from real-world (third-party) sources are transcribed into
  clean-room synthetic shapes before being committed; machine-minimized
  repros stay under `.corpus/`.

# HTML Parser

SAX-style streaming HTML parser with filter chain and DOM mutation support.
Ported from mod_pagespeed's `pagespeed/kernel/html/`.

## Key Files
- `html_parse.h` -- main parser class `HtmlParse` (~100 public methods)
- `html_filter.h` -- `HtmlFilter` base class for pipeline stages
- `html_element.h` -- `HtmlElement` (tags with attributes)
- `html_node.h` -- `HtmlNode` base, `HtmlLeafNode`, `HtmlCdataNode`, `HtmlCharactersNode`, etc.
- `html_event.h` -- event queue entries (StartElement, EndElement, Characters, etc.)
- `html_name.h` -- interned tag/attribute names (generated from `html_name.gperf`)
- `html_keywords.h` -- HTML entity/keyword lookup + tag close-style category tables
  (single source of truth for implicitly-closed / brief-termination tag sets)
- `html_lexer.h` -- tokenizer (internal to HtmlParse)
- `html_writer_filter.h` -- serializes parsed HTML back to text
- `empty_html_filter.h` -- no-op filter base for selective overrides
- `subresource_collector_filter.h` -- collects `<link>`, `<script>`, `<img>` URLs
- `doctype.h` -- DOCTYPE classification (exact-matching parser converged with
  mod_pagespeed 1.15; unrecognized doctypes degrade to `DocType::kUnknown`)
- `content_type.h` -- MIME type classification
- `html_parse_recorder.h` -- recording event sink for the
  differential harness (SPEC.md v1 parse event stream); drives
  `html_parse_probe` and `html_fuzz`. Corpus + goldens: vendored at
  `tools/html-parse-corpus/` (see its LOCAL_NOTES.md)

## Architecture

**Parser model**: `HtmlParse` tokenizes HTML into an event queue. Events flow through
a chain of `HtmlFilter`s. Filters can inspect and mutate the DOM during traversal.

**Lifecycle**: `StartParse(url)` → `ParseText(chunk)` (repeated) → `Flush()` → `FinishParse()`.
Supports streaming -- `Flush()` sends buffered events through the filter chain.

**Mutation**: Filters can insert, delete, move, and replace nodes during traversal.
Mutations are safe within the current flush window. Key methods:
`InsertNodeBeforeCurrent`, `InsertNodeAfterCurrent`, `DeleteNode`, `ReplaceNode`.

**Filter chain**: `AddFilter(filter)` before `StartParse`. Filters see events in order.
`HtmlWriterFilter` is typically the last filter (serializes output).

## Namespace
All symbols are in `net_instaweb::` (legacy from mod_pagespeed).

## Vendored kernel machinery (#1130, D1-pattern)

The HTML kernel is being vendored from the canonical 1.15 repo with the
same machinery as the D1 JS kernel (see `lib/js/CLAUDE.md` for the pattern
this clones):

- **Pin:** `lib/html/HTML_KERNEL_PIN` holds the canonical 1.15 commit SHA
  (first token of the first non-comment line — load-bearing contract) plus
  a keyword-keyed `floor <sha>` line for the pin-downgrade ratchet.
- **Sync:** `tools/sync-html-kernel.sh --source <path-to-1.15-checkout>`
  regenerates the vendored set from the pin (anchored `^#include` rewrites
  to `lib/html/`, `lib/html/compat/`, and `lib/base/` per the tool's fixed
  mapping table, plus a provenance banner; NO other source rewrites — a
  residue check fails the sync if canonical reintroduces the retired
  `StringPiece::CopyToString` member call). `--check` byte-compares without
  writing (used by CI).
- **Synced set (single-writer, NEVER hand-edit):** the 13-file scope
  (`html_lexer`, `html_parse`, `html_node`, `html_element`, `html_event`,
  `html_filter`, `html_keywords`, `html_name` (`.h`/`.gperf`), `doctype`,
  `html_writer_filter`, `empty_html_filter`, `content_type` — 24 files
  with headers) plus `html_parse_probe.cc` + `html_parse_recorder.h` (D1
  probe precedent: the D2 differential goldens gate both repos' probes, so
  the probe rides the pin) and the ledger `SYNCED_FILES.txt`.
- **Membership is explicit:** `tools/check-html-kernel-membership.sh` (run
  by the `html-kernel-differential` CI job and by the sync tool's
  `--check`) fails if any file exists under `lib/html/` that is neither
  listed in `SYNCED_FILES.txt` nor in the declared 2.0-owned set below.
  Unlike lib/js there is NO uniform vendored-name prefix — the optimizer-native
  files share the `html_*` shape — so both the carve-outs and this gate
  key on explicit name lists.
- **2.0-owned (editable):**
  - `compat/` — flat `.h`-only adaptation headers (`message_handler.h`,
    `google_url.h`, `string.h`, `string_util.h`, `logging.h`,
    `sparse_hash_map.h`, `string_hash.h`, `stl_util.h`, `timer.h`,
    `html_page_limit.h`)
    standing in for the canonical headers the vendored kernel includes.
    `compat/` is optimizer-owned and NEVER synced: the sync rewrites vendored
    includes to land here, never writes into it. New compat headers must
    be added to `//lib/html:compat` hdrs and included by
    `test/lib/html/compat_headers_test.cc` in the same change.
  - optimizer-native filters/parsers: `subresource_collector_filter`,
    `sitemap_parser`, `markdown_extractor_filter`, `robots_ai_directives`,
    `llms_txt_formatter`, `html_metadata_extractor`, `safe_entity_decode`,
    `csp_inline_policy`, and the `html_fuzz.cc` harness.
  - `BUILD`, `CLAUDE.md`, `HTML_KERNEL_PIN`, `SYNCED_FILES.txt`,
    `LICENSE.apache-2.0`.
- **DEFERRED decision (restore real URL validation):** `compat/google_url.h`
  is the Tier-1 policy seam, NOT a URL parser. Its default
  `NonEmptyUrlPolicy` (valid iff non-empty, identity normalization) keeps
  2.0 behavior byte-identical. Whether 2.0 restores real URL validation is
  undecided; the header's file-top comment is the normative record, and
  plugging in a real policy needs no canonical-code change.
- **Cross-compat-set safety:** `compat/string_util.h` INCLUDES
  `lib/js/compat/string_util.h` rather than redefining `StringPiece` /
  `strings::{StartsWith,EndsWith}` — `src/worker/worker.cc` includes both
  kernels' public headers, so both compat sets appear in one TU and any
  duplicate function definition (or a canonical-style `using absl::` decl)
  there is a hard compile error. Identical-alias headers (`string.h`) may
  stand alone; function-defining headers may not. `compat/logging.h`
  macros are per-name `#ifndef`-guarded like D1's.
- **Ratchet / byte / behavior gates:** the REQUIRED
  `html-kernel-differential` job in `pipeline-pr.yml` (called from `ci.yml`)
  runs on every pull request, merge queue run and dispatch, and on a push to
  main unless the merge queue already proved that commit (`CI_SCOPE`,
  `docs/ci/pipelines.md`). There is no path filter. It runs, in order: the
  membership gate; `tools/ci/check-floor-ratchet.sh --main-pin-path
  lib/html/HTML_KERNEL_PIN` (floor validity + forward-only vs origin/main,
  failing closed when a floor move cannot verify ancestry); the byte check,
  `tools/ci/check-kernel-bytes.sh`, which runs `tools/sync-html-kernel.sh
  --check` against a full clone of the canonical repository and compares
  the vendored files with the PINNED commit (never the canonical head, so a
  change landing in mod_pagespeed cannot fail an optimizer PR); and the
  vendored-goldens manifest check (behavior). Any byte difference, planted
  file or pin/floor/ancestry problem is RED, and the error repeats up to six
  of the sync tool's `DRIFT`, `RED` and `ERROR` lines. Network: the clone is
  retried a bounded number of times, with a per-attempt timeout where
  `timeout` is available, and its log is kept in the job output. After a
  successful clone the compare makes no network call, so a git fatal error
  there (exit 128) fails like any other tool failure. If the clone fails
  after the retries, the byte check only WARNS on a pull request that
  leaves the pin unchanged relative to origin/main; on a pin bump, on any
  other event (push, merge queue, dispatch), or when the pin or the
  origin/main pin cannot be read, it fails closed.
- **Availability dependency (#1117):** when the canonical 1.15 repo is
  unreachable from the runner, the byte check fails closed on every merge
  queue run and on any pin change, and the floor ratchet fails closed on
  any floor move. No pull request can land through the queue until
  reachability is restored. On the pull request run itself, a PR that
  leaves the pin alone only gets a warning. Scope: the required job
  verifies lineage (floor), membership, bytes against the pin, and
  behavior. It does not constrain an actor who edits the workflow or the
  check scripts themselves (the same trust class as the workflow files), or
  an admin pushing directly to main. `.github/CODEOWNERS` names
  `tools/ci/check-kernel-bytes.sh`, the sync and membership scripts and the
  pin files, so a change to them requests the owner's review; whether
  that review must complete before a merge is a repository setting, not
  something this job enforces. A diff
  touching these files is a stop for reviewers.
  `tools/ci/check-floor-ratchet.sh` and the workflow files are not listed.
- **Pin rule:** the pin must be a MERGED commit of the canonical 1.15
  master. The sync tool refuses a commit on an unmerged branch (exit 4) and
  one the canonical does not have (exit 3). There is no override.

## Carve-outs (and their limits)

Vendored files keep canonical 1.15 idiom/formatting: they are excluded from
clang-tidy TU selection and header diagnostics, clang-format, and the
pre-commit fixers — keyed on the EXPLICIT vendored-name list (the 13-file
scope; asserted by the sync tool's vendored-name invariant and made
non-spoofable by the membership gate). They are NOT excluded from the
license gate (the banner carries SPDX), sanitizers, tests, or the
differential job. The optimizer-native lib/html files and `compat/` are normal 2.0 code:
fully linted, formatted, and reviewed.

## Testing
```bash
bazel test //test/lib/html/...
```

## Gotchas
- Namespace is `net_instaweb::`, not `pagespeed::` -- this is legacy from mod_pagespeed.
- `HtmlName` uses a gperf-generated perfect hash; regenerate after modifying `html_name.gperf`.
- Flush window limits which nodes can be mutated -- nodes outside the window are read-only.
- `DISALLOW_COPY_AND_ASSIGN` macro still used (legacy); new code should use `= delete`.

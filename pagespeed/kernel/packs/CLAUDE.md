# pagespeed/kernel/packs/

Rule packs: the model, strict loader, matcher, value templates, decision
records and the streaming filter for operator-written rule packs (a JSON file
of head rules: canonical, title, description, hreflang, JSON-LD). Portable
kernel code in namespace `pagespeed::packs`. It depends only on the HTML kernel
(`//pagespeed/kernel/html`), abseil, RE2 and jsoncpp, so it can be reused outside
a rewriter. `RulePackFilter` runs a pack over an `HtmlParse`. The rewriter
runs it as the `rule_packs` filter (off by default and in no rewrite level):
`net/instaweb/rewriter/rule_pack_host.cc` holds the `PackHost` over a
`RewriteDriver` and the driver-side filter that fetches the pack per
document; `pagespeed/system/rule_pack_store.cc` is the per-process pack cache
and `rule_pack_decision_log.cc` the decision log. The canonical, title and
description kinds plan at head close, with hreflang; jsonld plans when the
document ends.

## Files

| File | Purpose |
|------|---------|
| `pack.{h,cc}` | Pack model, every limit (`k*` constants), value checks, the `pack` object incl. the reserved `license` |
| `pack_loader.{h,cc}` | `LoadPack()` / `LoadPackFile()`: strict jsoncpp parse, validation, error text `<source>: <json path>: <reason>` |
| `json_util.{h,cc}` | Helpers over `Json::Value` for the loader and the kinds; `CheckUtf8()` |
| `rule_kind.h` | `RuleKind`, the per-kind handler interface: `ParseValue` (load), `Observe`, `Plan`, `OnLateElement`, `BuildInsert` (page time) |
| `kind_table.inc` | The ONE list of kinds, in application order (`RULE_KIND(name, class, header)`) |
| `kind_registry.{h,cc}` | Expands the table into static handler instances; `FindKind()`, `KindIndex()`, `AllKinds()` |
| `kinds/*_kind.{h,cc}` | One handler class per kind |
| `kinds/head_singleton_kind.{h,cc}` | The keep / repair / replace ladder shared by canonical, title and description |
| `kinds/hreflang_kind.{h,cc}` | The alternate-language cluster (several elements, decided at head close) |
| `kinds/jsonld_kind.{h,cc}` | JSON-LD blocks anywhere in the document, decided at the end of the document |
| `element_fact.h` | One element of a kind found in a document |
| `planner_common.{h,cc}` | Page-time types (`PlanOp`, `RulePlan`, `PlanInput`, `PlanState`, `ElementView`) and pure helpers; independent of the registry |
| `planner.{h,cc}` | `BuildPlan()`: the pure plan of one decision point; `ParsePageUrl()` |
| `signature_format.{h,cc}` | The text formats of pack signing: the `.sig` sidecar parser, trust-key decoding, `PackTrust` (keys + `require_signed`), `SignatureStatus` and its log phrases. No crypto (verification is `pagespeed/system/rule_pack_signature`) |
| `pack_host.{h,cc}` | `PackHost`: what the filter needs from its server (headers, counters, the decision sink) |
| `rule_pack_filter.{h,cc}` | `RulePackFilter`, the streaming `EmptyHtmlFilter` |
| `rule_pack_filter_fuzz.cc` | Filter fuzz harness (fixed pack, fuzzed HTML, fuzzed flush windows); seeds in `tools/rule-pack-filter-corpus/` |
| `rule_value_parser.{h,cc}` | The `value` object shape checks all kinds share; each kind passes a `ValueSpec` |
| `matcher.{h,cc}` | Host and path matching, rule selection, table and cluster lookup |
| `template.{h,cc}` | Placeholder templates: parse, expand, escape |
| `url_norm.{h,cc}` | URL equality normalization, hreflang code validation |
| `decision.{h,cc}` | Decision records and the decision-log line encoder (no I/O) |
| `page_facts.h` | What an HTML traversal collects per kind (indexed by `KindIndex()`) |
| `entity_decode.{h,cc}` | `IsWellFormedUtf8()` and UTF-8-safe entity decoding |
| `link_header.{h,cc}` | RFC 8288 `Link` header splitter, canonical link-value writer and `PlanCanonicalLinkHeader()`, the pure plan of the response-header action |
| `engine_version.{h,cc}` | The version `engine_min` is compared against (`MOD_PAGESPEED_VERSION_STRING`) |
| `pack_loader_fuzz.cc` | Fuzz harness (replay + libFuzzer); seeds in `tools/pack-loader-corpus/` |

## Rules for changes

- **Reserved names**: the top-level key `signature` and the `pack.license` keys
  are format decisions (the transform-pack format spec, section 3.4); do not add or repurpose keys
  there without a spec change.
- **Adding a kind**: a handler pair under `kinds/`, one `RULE_KIND(...)` row in
  `kind_table.inc`, an include in `kind_registry.cc`, the `BUILD` entries. The
  loader, matcher and facts pick it up from the registry; no `switch` over
  kinds exists. A test in `kind_registry_test.cc` pins the order.
- **JSON**: jsoncpp through `pagespeed/kernel/base/json.h`. The loader's reader
  is `CharReaderBuilder::strictMode` plus `rejectDupKeys` and
  `stackLimit = kMaxJsonDepth`. jsoncpp does not validate UTF-8, so
  `CheckUtf8()` walks every key and string right after parsing; a new place that
  accepts text must be covered by that walk (it is, as long as the text comes
  from the parsed tree).
- **Errors** are `kInvalidArgument` with `<json path>: <reason>`; the loader adds
  the source name once, at the outermost level.
- **Limits** live in `pack.h`. The fuzz harness checks only the rule count, a
  non-empty site list and a non-null kind on every rule of a loaded pack.
- jsoncpp accepts raw control bytes inside strings (nlohmann rejected them).
  Nothing new is reachable: title and description values still reject control
  characters in their own checks, and other text is matched literally.
- **`HtmlKeywords::Init()`** (done once per process by `ProcessContext`) must
  have run before `SafeDecodeHtmlEntities()`.
- No `net_instaweb` namespace in new code here.
- **Dependency order** (Bazel rejects a cycle): `rule_kind` ← `decision` ←
  `planner_common` ← `kinds` ← `kind_registry` ← `matcher` ← `planner` ←
  `rule_pack_filter`. A kind may not use the registry or the matcher; whatever
  it needs from them (the table value, a cluster) comes in `PlanInput`.

## Streaming (RulePackFilter)

The module parses in flush windows: each filter runs over one window of events,
then `Flush()`; a node whose start or end tag was in an earlier window is no
longer rewritable (`HtmlParse::IsRewritable`). The filter is built around that:

- **Decision points.** The head kinds decide at the first of `</head>`
  (written or implied), `<body>`, and `EndDocument`, with every fact observed
  so far, including elements between the logical head end and `</head>`
  (`in_head = false`). A `<head>` that first appears after `<body>` is not a
  head: the page is skipped as `no_head`.
- **Atomicity.** Before mutating, every enforcing op is checked: targets and
  their text nodes must be rewritable; an insert needs its anchor (the start of
  the logical-head-end element in the current window, else
  `CanAppendChild(head)`). One failure drops the whole plan of that point;
  the rules that would have acted, at that point or on a late element,
  record `not_rewritable` with action `none` (as does a late element that
  could not be removed). Inserts into a head
  split by a flush therefore still work; edits of sent elements fail closed.
- **Document end.** A kind with `DecisionPoint::kDocumentEnd` (jsonld) is
  planned in `EndDocument` with every element of its kind in view and the head
  values the document ends with: `PageContext::head_plan` carries the head
  plan, and `RuleKind::FinalValue()` of the head kinds says what each ends
  with (`PlanInput::final_canonical/_title/_description`). An enforcing rule
  quotes only what is really in the document (a dropped or report-only head
  plan counts for nothing); a report-only rule quotes the planned value. A
  new element is appended to `<body>` while its end is still in the current
  window (`CanAppendChild`), else to `<html>`, else the plan is dropped as
  `not_rewritable`. Removals and in-place rewrites go through the element's
  own events, so a block an earlier window already sent cannot be touched:
  the whole document-end plan is dropped, exactly as for the head. The two
  plans are independent: one can apply while the other drops. The pages
  budget (`max_added_bytes`) is shared by the two points.
- **JSON text** read from a page or expanded from a template goes through
  `ParseStrictJson()` (`json_util.h`): browser-grade grammar, well-formed
  UTF-8, no raw control character in a string, nesting capped at
  `kMaxJsonLdDepth`; an expanded template also rejects a repeated key. A
  block over `kMaxJsonLdScanBytes` or past the nesting cap is never parsed,
  rewritten or removed.
- **Late elements** (a head kind seen after its decision point) go through
  `RuleKind::OnLateElement`: removed under repair/replace, exact duplicates
  removed under keep, and under keep after the pack inserted the kind, removed
  as `late_conflict` (a deliberate difference from a whole-document planner,
  which sees them before deciding). Decisions are recorded at
  `EndDocument`; except for that keep case, they equal the ladder's decision
  over the whole document.
- **Nothing is cached between windows except facts.** Element pointers stay
  allocated until the parse ends, but their attributes are freed after their
  window: copy values into `ElementFact` when observing, never read an
  element after its window.
- **Per document state** is reset in `DetermineEnabled()` (the parser calls it
  before the first event); the URL comes from `HtmlParse::url()`, so one filter
  serves a parser that is reused across requests.
- Report mode never changes the document; with debug comments off its output
  is byte-identical to the same parse without the filter (the fuzz harness
  checks it in fuzzed flush windows). With debug comments on it adds
  `would=` comments where an enforcing rule would act, and nothing else.
- **`link_header: match`** (canonical rules only; the loader refuses it on
  other kinds). The head plan records the canonical the document ends with
  (`RulePlan::final_norm` / `final_url`). Right after the head is applied the
  filter reads the response `Link` header through `PackHost::response_headers()`
  and `PlanCanonicalLinkHeader()` says what to do: add `<url>; rel="canonical"`
  when the header has none and the document's canonical is the rule's value;
  nothing when they agree; on a conflict `keep` only reports
  (`header_conflict`) while `repair` and `replace` drop the conflicting entries
  (every other link-value stays, a `rel="canonical preload"` loses only the
  canonical token) and add the document's. Once the first flush window has gone
  out the action is `none` / `headers_flushed` (and `rule_pack_headers_flushed`
  moves). A host without response headers (decided when the document begins) records
  nothing; a line the splitter could not read in full is never rebuilt (a
  conflict on it is reported, the header left alone); a dropped head plan has no header action. Report mode reads the header and never writes it.
  Header decisions are separate decisions of the rule (`PackDecision::header`,
  `"header":true` in the log line), right after the rule's document decisions.
- **Debug comments** (`PackHost::debug_comments()`, the `debug` filter on a
  server) read `rule=<id> action=<a> mode=<m>` before each change, and
  `rule=<id> would=<a> mode=report` in report mode.
- **The pack is fixed per filter.** A host that can see a new pack between
  documents builds a new filter for it (the rewriter does, per document), so
  the decisions a host logs always belong to the pack the filter holds.

## Testing

```bash
bazel test //test/pagespeed/kernel/packs/...
```

Tests live in `test/pagespeed/kernel/packs/`; `test_json.h` is a small mutable
JSON value used to build and break packs, `test_pack_host.h` a `PackHost`
without response headers.

The fixture cases under `testdata/edge-seo/fixtures/` are shared with other
engines that run the same packs: never edit their `expected.html` /
`expected.decisions.jsonl`. `fixture_runner_test` runs every case whole and in
five chunkings and checks the properties listed in its header. Where this
filter deliberately differs, the case gets `expected.module.html` /
`expected.module.decisions.jsonl` (write them with
`bazel run //test/pagespeed/kernel/packs:fixture_update -- <case>`) and must
be listed with its reason in `ModuleOverrides()` in `fixture_runner.cc` (one list for
every runner). The same cases also run through a real `RewriteDriver` in
`test/net/instaweb/rewriter/rule_pack_fixture_test.cc` (three chain variants,
whole and chunked), and `test/system/system/test_rule_packs.py` covers a live
server per port.

The chunked property treats the two decision points independently: each
applies whole or drops whole. So a chunking may leave the head's changes in
place while the JSON-LD append drops (`not_rewritable`, only the document-end
rules), and the appended blocks may sit in front of `</html>` instead of
`</body>` when `</body>` was already sent. The converse is allowed too: the
head plan drops (it edits an element a flush already sent) while the JSON-LD
append applies.
`ChunkedOutcomeAllowed()` checks each of these exactly, against a reference run
of the pack without the other decision point's rules (`HeadOnlyPack()`,
`DocumentEndOnlyPack()`). The appended blocks are relocated in front of
`</html>` only for the whole-document page and for the head-dropped page
(`DocumentEndOnlyPack()` reference), never for the head-only reference, which
has no appended blocks.

Per-page cost: `bazel run -c opt //test/pagespeed/kernel/packs:rule_pack_filter_benchmark`. The example pack is
`test/pagespeed/kernel/packs/testdata/edge-seo/pack.example.json`. Its
`engine_min` must not be newer than the current module version.

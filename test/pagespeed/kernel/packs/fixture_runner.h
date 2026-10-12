// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_PAGESPEED_KERNEL_PACKS_FIXTURE_RUNNER_H_
#define TEST_PAGESPEED_KERNEL_PACKS_FIXTURE_RUNNER_H_

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/planner_common.h"

// Runs the rule-pack fixtures through HtmlParse + RulePackFilter +
// HtmlWriterFilter: the filter at the kernel level, without a rewriter.

namespace pagespeed::packs {

// One response header line.
using HeaderLine = std::pair<std::string, std::string>;

struct RunResult {
  std::string html;
  std::vector<PackDecision> decisions;
  bool modified = false;
  SkipReason skip = SkipReason::kNone;
  // The response headers after the run, and whether the filter changed them.
  // Empty unless the run was given origin headers.
  std::vector<HeaderLine> headers;
  bool headers_modified = false;
};

struct RunOptions {
  Mode global_mode = Mode::kEnforce;
  bool debug_comments = false;
  // False: the same parse without the filter (the "unchanged" reference; the
  // writer re-serializes some tag whitespace).
  bool with_filter = true;
  // Byte offsets of the input after which the parser is flushed, ascending.
  std::vector<size_t> flush_after;
  // The response headers the origin sent. Unset: the host has no headers.
  std::optional<std::vector<HeaderLine>> origin_headers;
};

// Runs `html` (served at `url`) through the filter.
absl::StatusOr<RunResult> RunPackOnHtml(std::shared_ptr<const Pack> pack,
                                        std::string_view url,
                                        std::string_view html,
                                        const RunOptions& options);

// A fixture directory: input.html, request.json, expected.html,
// expected.decisions.jsonl, and optionally expected.module.html /
// expected.module.decisions.jsonl where the module deliberately differs.
// request.json: {"url": "...", "mode": "enforce"|"report" (the directive
// mode, default enforce), "debug": bool, "pack": "file next to pack.json",
// "flush_every": N (flush the parser every N bytes, also in the whole-document
// runs; default none), "origin_headers": [["Name", "value"], ...] (the
// response headers of the origin, in order; the host has none when absent)}.
// A case with origin_headers also has expected.headers: the response headers
// after the run, one "Name: value" line each.
struct FixtureCase {
  std::string name;
  std::string dir;
};

// The case directories under `fixtures_dir`, sorted by name.
std::vector<FixtureCase> ListFixtureCases(const std::string& fixtures_dir);

// Decision lines of a run, one JSON object per line, each ending in '\n'.
// `force_report` rewrites every mode to "report", the shape a report-only
// run must produce.
std::string DecisionsToJsonl(std::string_view host, std::string_view path,
                             const std::vector<PackDecision>& decisions,
                             bool force_report);

// One way of cutting the input into flush windows.
struct Chunking {
  std::string name;  // "every-16", "after-head", ...
  std::vector<size_t> flush_after;
};

// Every 16, 64 and 256 bytes; right after the first <head ...>; right before
// the first </head>. (The last two only when the input has them.)
std::vector<Chunking> ChunkingsFor(std::string_view html);

struct ChunkedRun {
  Chunking chunking;
  RunResult enforce;    // directive mode as the request says
  RunResult report;     // forced report-only
  RunResult roundtrip;  // without the filter, same chunking
};

// Everything the runner derives for one case.
struct CaseRun {
  std::string input_html;
  std::string url;
  std::string host;
  std::string path;                      // with "?query" when there is one
  bool debug = false;                    // the request turns debug comments on
  Mode directive_mode = Mode::kEnforce;  // the request's directive mode
  std::string pack_file = "pack.json";   // next to the fixtures
  // The offsets the request's flush_every puts the flushes at (also in the
  // whole-document runs).
  std::vector<size_t> flush_cuts;
  // The request has origin_headers (and the case an expected.headers).
  bool has_origin_headers = false;
  std::vector<HeaderLine> origin_headers;
  RunResult enforce;        // directive mode as the request says
  RunResult again;          // the same run once more (determinism)
  RunResult report;         // forced report-only
  RunResult roundtrip;      // without the filter
  RunResult head_only;      // with the rules that decide at the end removed
  RunResult document_only;  // with the rules that decide in the head removed
  RunResult idempotent;     // `enforce` output run again
  std::vector<ChunkedRun> chunked;
};

// The cases whose module result differs from the shared expected files on
// purpose, and why. Every runner that compares against the expected files
// uses this one list, so a new difference fails everywhere until it is
// reviewed and listed.
const std::map<std::string, std::string>& ModuleOverrides();

// The chunked property: a run split into flush windows gives either the
// whole-document result (`whole`), or the output without the filter
// (`chunked_roundtrip_html`, the same windows with no filter) with the same
// rules decided as `none` / `not_rewritable`. Never anything else. On a
// violation, `why` (when non-null) is filled in.
//
// Outputs are compared byte for byte unless `same_html` is given: a chain with
// other filters in it need not give the same bytes in every window layout, and
// supplies the equivalence it can promise (the same head elements).
//
// The kinds that decide at the end of the document add two outcomes, both
// exact: (a) when </body> was already sent the appended blocks sit in front of
// </html> instead (the whole-document page with those blocks moved); (b) when
// the body and the html element were both sent, that plan drops while the
// head applied: `head_only_html` (the whole-document run of the pack without
// the end-of-document rules) is the page, and the document-end decisions that
// would have acted say not_rewritable. (b) is checked only when
// `head_only_html` is given.
//
// And the converse, (c): the head plan drops (it edits an element that was
// already sent) while the end-of-document plan applies. `document_only_html`
// (the whole-document run of the pack without the head rules, or that page
// with its appended blocks moved in front of </html>) is the page, the
// document-end decisions are the whole-document ones, and every head decision
// that would have acted says not_rewritable. (c) is checked only when
// `document_only_html` is given.
bool ChunkedOutcomeAllowed(
    const RunResult& whole, const RunResult& chunked,
    std::string_view chunked_roundtrip_html, std::string* why,
    const std::function<bool(const std::string&, const std::string&)>&
        same_html = nullptr,
    const std::string* head_only_html = nullptr,
    const std::string* document_only_html = nullptr);

// `pack` without the rules of the kinds that decide at the end of the
// document: what the head alone does.
std::shared_ptr<const Pack> HeadOnlyPack(const Pack& pack);

// `pack` without the rules of the kinds that decide in the head: what the end
// of the document alone does.
std::shared_ptr<const Pack> DocumentEndOnlyPack(const Pack& pack);

// "rule:action/reason " for each decision, for failure messages.
std::string DescribeDecisions(const std::vector<PackDecision>& decisions);

absl::StatusOr<CaseRun> RunCase(const std::string& fixtures_dir,
                                const FixtureCase& c);

struct Expected {
  std::string html;
  std::string decisions;
  // expected.headers; empty when the case has none.
  std::string headers;
  // The module-specific files were used.
  bool module_html = false;
  bool module_decisions = false;
};

// The expected output of a case: the module files where they exist, else
// the shared ones.
absl::StatusOr<Expected> ReadExpected(const FixtureCase& c);

// True when the case has a module-specific expected file.
bool HasModuleOverride(const FixtureCase& c);

// Writes expected.module.html and expected.module.decisions.jsonl of `c` from
// a run. Never touches the shared expected files.
absl::Status WriteModuleExpected(const FixtureCase& c, const CaseRun& run);

// "Name: value\n" for each line.
std::string HeadersToText(const std::vector<HeaderLine>& headers);

// The response-header part of the chunked property. The header decisions of a
// chunked run are those of the whole-document run, except that any of them may
// instead say `none` / `headers_flushed` (and the rule may say so although the
// whole-document run had nothing to say); the headers are then the origin's,
// and otherwise the whole-document run's. A head plan that was dropped
// (`skip` not_rewritable) has no header decisions and leaves the headers
// alone; a dropped document-end plan does not, so a run with only that is
// held to the full rule.
bool ChunkedHeadersAllowed(const RunResult& whole, const RunResult& chunked,
                           const std::vector<HeaderLine>& origin);

// The decisions about the document (not about response headers).
std::vector<PackDecision> DocumentDecisions(
    const std::vector<PackDecision>& decisions);

// Writes expected.html, expected.decisions.jsonl (and expected.headers when
// the case has origin headers) of a case that exists only in this module.
absl::Status WriteNewCaseExpected(const FixtureCase& c, const CaseRun& run);

// Reads a whole file. On Windows the path is made absolute and long-path
// prefixed, so a fixture deeper than MAX_PATH still opens. The error message
// names the path with forward slashes.
absl::StatusOr<std::string> ReadFile(const std::string& path);

// The path to open for `path` (see ReadFile()).
std::filesystem::path FilePath(const std::string& path);

// `html` without the filter's debug comments ("<!-- pagespeed-pack ... -->").
// With debug comments on, report mode marks what it would change; that and
// nothing else may differ from the parse without the filter.
std::string StripPackComments(std::string_view html);

}  // namespace pagespeed::packs

#endif  // TEST_PAGESPEED_KERNEL_PACKS_FIXTURE_RUNNER_H_

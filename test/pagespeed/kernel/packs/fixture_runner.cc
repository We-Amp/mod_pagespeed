// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "test/pagespeed/kernel/packs/fixture_runner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/html/html_writer_filter.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/planner.h"
#include "pagespeed/kernel/packs/rule_pack_filter.h"
#include "test/pagespeed/kernel/packs/test_pack_host.h"

namespace pagespeed::packs {

namespace fs = std::filesystem;

fs::path FilePath(const std::string& path) {
  fs::path p = fs::path(path).lexically_normal();
#ifdef _WIN32
  // The runfiles tree sits deep under the output base, and the longer case
  // names push a fixture file past MAX_PATH (260). The \\?\ prefix lifts
  // that limit; it needs an absolute path with backslashes only.
  std::error_code ec;
  fs::path abs = fs::absolute(p, ec);
  if (!ec) {
    abs = abs.lexically_normal();
    abs.make_preferred();
    const std::wstring& w = abs.native();
    if (w.rfind(L"\\\\?\\", 0) != 0) {
      if (w.rfind(L"\\\\", 0) == 0) {
        p = fs::path(L"\\\\?\\UNC\\" + w.substr(2));
      } else {
        p = fs::path(L"\\\\?\\" + w);
      }
    } else {
      p = abs;
    }
  }
#endif
  return p;
}

absl::StatusOr<std::string> ReadFile(const std::string& path) {
  std::ifstream in(FilePath(path), std::ios::binary);
  if (!in) {
    return absl::NotFoundError(absl::StrCat(
        "cannot read ", fs::path(path).lexically_normal().generic_string()));
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

absl::StatusOr<RunResult> RunPackOnHtml(std::shared_ptr<const Pack> pack,
                                        std::string_view url,
                                        std::string_view html,
                                        const RunOptions& options) {
  return RunPacksOnHtml({std::move(pack)}, url, html, options);
}

absl::StatusOr<RunResult> RunPacksOnHtml(
    std::vector<std::shared_ptr<const Pack>> packs, std::string_view url,
    std::string_view html, const RunOptions& options) {
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  TestPackHost host(options.debug_comments);
  ClaimSet claims;
  host.set_claims(&claims);
  if (options.origin_headers.has_value()) {
    host.SetResponseHeaders(*options.origin_headers);
  }

  std::vector<std::unique_ptr<RulePackFilter>> filters;
  if (options.with_filter) {
    std::set<std::string> ids;
    for (std::shared_ptr<const Pack>& pack : packs) {
      // (A server refuses a file whose pack id is already loaded.)
      if (!ids.insert(pack->info.id).second) continue;
      filters.push_back(std::make_unique<RulePackFilter>(
          &parser, std::move(pack), options.global_mode, &host));
      parser.AddFilter(filters.back().get());
    }
  }

  RunResult result;
  net_instaweb::StringWriter writer(&result.html);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);

  if (!parser.StartParse(url)) {
    return absl::InvalidArgumentError("parse did not start");
  }
  size_t at = 0;
  for (size_t cut : options.flush_after) {
    if (cut <= at || cut >= html.size()) continue;
    parser.ParseText(html.substr(at, cut - at));
    parser.Flush();
    // The first flush window sends the response headers.
    host.FlushHeaders();
    at = cut;
  }
  parser.ParseText(html.substr(at));
  parser.FinishParse();
  for (size_t i = 0; i < filters.size(); ++i) {
    const RulePackFilter& filter = *filters[i];
    for (const PackDecision& d : filter.decisions()) {
      result.decisions.push_back(d);
      result.decision_pack.push_back(i);
    }
    result.modified = result.modified || filter.modified();
    if (result.skip == SkipReason::kNone) result.skip = filter.skip_reason();
    result.headers_modified =
        result.headers_modified || filter.headers_modified();
  }
  if (!filters.empty()) {
    // What the host was told is what the filters recorded (the packs report
    // in turn, so the host's order may interleave them).
    std::vector<PackDecision> told = host.decisions();
    for (const PackDecision& d : result.decisions) {
      auto it = std::find(told.begin(), told.end(), d);
      if (it == told.end()) {
        return absl::InternalError("host decisions differ from the filter's");
      }
      told.erase(it);
    }
    if (!told.empty()) {
      return absl::InternalError("host decisions differ from the filter's");
    }
    result.claimed_count = host.count(Counter::kDecisionsClaimed);
  }
  if (options.origin_headers.has_value()) {
    result.headers = host.headers().lines();
  }
  return result;
}

std::string HeadersToText(const std::vector<HeaderLine>& headers) {
  std::string out;
  for (const auto& [name, value] : headers) {
    absl::StrAppend(&out, name, ": ", value, "\n");
  }
  return out;
}

std::vector<PackDecision> DocumentDecisions(
    const std::vector<PackDecision>& decisions) {
  std::vector<PackDecision> out;
  for (const PackDecision& d : decisions) {
    if (!d.header) out.push_back(d);
  }
  return out;
}

bool ChunkedHeadersAllowed(const RunResult& whole, const RunResult& chunked,
                           const std::vector<HeaderLine>& origin) {
  bool any_header = false;
  bool not_rewritable = false;
  for (const PackDecision& d : chunked.decisions) {
    any_header = any_header || d.header;
    // Only a dropped head plan takes the early path: a dropped document-end
    // plan leaves the head, and with it every header decision, in place.
    not_rewritable =
        not_rewritable ||
        (!d.header && d.reason == Reason::kNotRewritable && d.kind != nullptr &&
         d.kind->decision_point() == DecisionPoint::kHeadEnd);
  }
  if (!any_header && not_rewritable) {
    // A dropped head plan leaves the headers alone and decides nothing about
    // them.
    return chunked.headers == origin;
  }
  std::map<std::string, PackDecision> want;
  for (const PackDecision& d : whole.decisions) {
    if (d.header) want[d.rule_id] = d;
  }
  bool changes = false;
  std::set<std::string> seen;
  for (const PackDecision& got : chunked.decisions) {
    if (!got.header) continue;
    seen.insert(got.rule_id);
    // Once the headers went out the action cannot be decided at all, so the
    // decision says so, whatever the whole document would have done (even
    // nothing).
    if (got.action == Action::kNone && got.reason == Reason::kHeadersFlushed) {
      continue;
    }
    auto it = want.find(got.rule_id);
    if (it == want.end() || !(it->second == got)) return false;
    if (got.action != Action::kNone && got.mode == Mode::kEnforce) {
      changes = true;
    }
  }
  // A decision of the whole-document run can only be missing where the
  // headers were flushed, and that is recorded.
  for (const auto& [rule, d] : want) {
    if (seen.count(rule) == 0) return false;
  }
  return chunked.headers == (changes ? whole.headers : origin);
}

std::vector<FixtureCase> ListFixtureCases(const std::string& fixtures_dir) {
  std::vector<FixtureCase> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fixtures_dir, ec)) {
    if (!e.is_directory()) continue;
    // Forward slashes throughout, so paths built from `dir` are not mixed.
    out.push_back({e.path().filename().string(), e.path().generic_string()});
  }
  std::sort(out.begin(), out.end(),
            [](const FixtureCase& a, const FixtureCase& b) {
              return a.name < b.name;
            });
  return out;
}

std::string DecisionsToJsonl(std::string_view host, std::string_view path,
                             const std::vector<PackDecision>& decisions,
                             bool force_report) {
  std::string out;
  for (PackDecision d : decisions) {
    if (force_report) d.mode = Mode::kReport;
    absl::StrAppend(&out, DecisionToJsonLine(host, path, d), "\n");
  }
  return out;
}

// The cases whose module result differs from the shared expected files on
// purpose, and why.
// A JSON-LD block the filter adds goes to the end of <body>: the kind decides
// when the document ends, with every block of the page (head or body) in
// view, so the page's own blocks in <body> count and no second Organization is
// added next to a footer one. The expected files put it at the end of <head>.
constexpr char kJsonLdAtDocumentEnd[] =
    "The block is appended to <body> before </body> (the kind decides at the "
    "end of the document), not added at the end of <head>.";

std::shared_ptr<const Pack> HeadOnlyPack(const Pack& pack) {
  auto out = std::make_shared<Pack>(pack);
  std::erase_if(out->rules, [](const Rule& r) {
    return r.kind != nullptr &&
           r.kind->decision_point() == DecisionPoint::kDocumentEnd;
  });
  return out;
}

std::shared_ptr<const Pack> DocumentEndOnlyPack(const Pack& pack) {
  auto out = std::make_shared<Pack>(pack);
  std::erase_if(out->rules, [](const Rule& r) {
    return r.kind != nullptr &&
           r.kind->decision_point() == DecisionPoint::kHeadEnd;
  });
  return out;
}

std::shared_ptr<const Pack> ClaimOnlyPack(const Pack& pack, unsigned dropped) {
  auto out = std::make_shared<Pack>(pack);
  for (Rule& r : out->rules) {
    if (r.kind == nullptr) continue;
    const DecisionPoint point = r.kind->decision_point();
    if ((point == DecisionPoint::kHeadEnd && (dropped & kHeadDropped) != 0) ||
        (point == DecisionPoint::kDocumentEnd &&
         (dropped & kDocumentDropped) != 0)) {
      r.enforce = false;
    }
  }
  return out;
}

const std::map<std::string, std::string>& ModuleOverrides() {
  static const auto* overrides = new std::map<std::string, std::string>{
      {"canonical-in-body-keep",
       "The head is decided when it closes, before the body canonical is "
       "seen, so keep inserts one. The body canonical then arrives late and "
       "is removed (late_conflict): leaving it would give the page two "
       "canonicals where the input had one usable answer for a crawler, "
       "and search engines ignore a canonical outside the head anyway."},
      {"title-in-body-keep",
       "Same as canonical-in-body-keep for <title>: inserted at head close, "
       "the body title removed as a late conflict."},
      {"global-pack-debug-comments",
       "Debug comments also name the rule's effective mode (mode=enforce); "
       "in report mode the same place gets a would= comment instead."},
      {"global-head-after-body",
       "The head kinds decide at <body>; no <head> has been seen by then, so "
       "the page is skipped as no_head and left alone. An HTML parser "
       "ignores a <head> after <body> and moves its children into the body, "
       "so inserting there would not put anything into the head."},
      {"hreflang-debug-comment",
       "Debug comments also name the rule's effective mode (mode=enforce); "
       "in report mode the same place gets a would= comment instead."},
      {"jsonld-debug-comment", kJsonLdAtDocumentEnd},
      {"jsonld-graph-types", kJsonLdAtDocumentEnd},
      {"jsonld-inject-template", kJsonLdAtDocumentEnd},
      {"jsonld-invalid-repair", kJsonLdAtDocumentEnd},
      {"jsonld-noscript-ignored", kJsonLdAtDocumentEnd},
      {"jsonld-other-type-insert-keep", kJsonLdAtDocumentEnd},
      {"jsonld-placeholders-existing-values", kJsonLdAtDocumentEnd},
      {"jsonld-placeholders-final-values", kJsonLdAtDocumentEnd},
      {"jsonld-repair-absent", kJsonLdAtDocumentEnd},
      {"jsonld-repair-empty", kJsonLdAtDocumentEnd},
      {"jsonld-replace-absent", kJsonLdAtDocumentEnd},
      {"jsonld-script-close-escape", kJsonLdAtDocumentEnd},
      {"jsonld-type-parameter-ignored", kJsonLdAtDocumentEnd},
      {"jsonld-unicode", kJsonLdAtDocumentEnd},
  };
  return *overrides;
}

std::string DescribeDecisions(const std::vector<PackDecision>& decisions) {
  std::string out;
  for (const PackDecision& d : decisions) {
    absl::StrAppend(&out, d.rule_id, ":", ActionName(d.action), "/",
                    ReasonName(d.reason), " ");
  }
  return out;
}

namespace {

constexpr std::string_view kBlockStart =
    "<script type=\"application/ld+json\">";

// Cuts up to `max` script blocks that sit directly in front of `*page`'s
// offset `at` (each with the debug comment in front of it, if any) and
// returns them in document order; the page loses them. Stops at anything that
// is not exactly one JSON-LD block.
std::string CutBlocksBefore(std::string* page, size_t at, size_t max,
                            size_t* count) {
  static constexpr std::string_view kComment = "<!-- pagespeed-pack ";
  std::string cut;
  *count = 0;
  while (*count < max && at >= 9 &&
         page->compare(at - 9, 9, "</script>") == 0) {
    const size_t start = page->rfind(kBlockStart, at);
    // (Exactly one block: its own end tag is the one in front of `at`.)
    if (start == std::string::npos ||
        page->find("</script>", start) != at - 9) {
      break;
    }
    size_t from = start;
    const size_t comment = page->rfind(kComment, start);
    if (comment != std::string::npos) {
      const size_t end = page->find("-->", comment);
      if (end != std::string::npos && end + 3 == start) from = comment;
    }
    cut.insert(0, *page, from, at - from);
    page->erase(from, at - from);
    at = from;
    ++*count;
  }
  return cut;
}

// `whole` with its last `k` blocks in front of </body> moved to the position
// in front of </html>, where the filter puts them when </body> was already
// sent. Empty when the page has no such blocks or no </body> ... </html>.
std::optional<std::string> RelocateBlocks(const std::string& whole, size_t k) {
  const size_t body_end = whole.rfind("</body>");
  const size_t html_end = whole.rfind("</html>");
  if (body_end == std::string::npos || html_end == std::string::npos ||
      body_end > html_end) {
    return std::nullopt;
  }
  std::string page = whole;
  size_t count = 0;
  const std::string moved = CutBlocksBefore(&page, body_end, k, &count);
  if (count != k) return std::nullopt;
  page.insert(html_end - moved.size(), moved);
  return page;
}

// `d` as the filter records it when its plan could not be applied.
PackDecision AsNotRewritable(PackDecision d) {
  d.action = Action::kNone;
  d.reason = Reason::kNotRewritable;
  d.before_hash.clear();
  d.after.clear();
  d.removed.clear();
  return d;
}

}  // namespace

bool ChunkedOutcomeAllowed(
    const RunResult& whole, const RunResult& chunked,
    std::string_view chunked_roundtrip_html, std::string* why,
    const std::function<bool(const std::string&, const std::string&)>&
        same_html,
    const std::string* head_only_html, const std::string* document_only_html) {
  auto same = [&](const std::string& a, const std::string& b) {
    return same_html ? same_html(a, b) : a == b;
  };
  // (The response-header decisions have their own rule,
  // ChunkedHeadersAllowed().)
  const std::vector<PackDecision> got_doc =
      DocumentDecisions(chunked.decisions);
  const std::vector<PackDecision> want_doc = DocumentDecisions(whole.decisions);
  if (same(chunked.html, whole.html) && got_doc == want_doc) return true;
  // The head and the end of the document are decided one after the other,
  // each applied whole or dropped whole, and a kind that decides at the end of
  // the document appends to <body> only while </body> is still in the current
  // flush window, else to <html>.
  //
  // (a) </body> was already sent: the whole-document page with the blocks the
  // filter appended moved from in front of </body> to in front of </html>.
  if (got_doc == want_doc) {
    for (size_t k = 1;; ++k) {
      std::optional<std::string> moved = RelocateBlocks(whole.html, k);
      if (!moved.has_value()) break;
      if (same(chunked.html, *moved)) return true;
    }
  }
  // (b) The end of the document dropped (the body and the html element were
  // both sent) while the head applied: the page is the head-only run's, every
  // head decision is the whole-document one, and every document-end
  // decision that would have acted says not_rewritable.
  if (head_only_html != nullptr && same(chunked.html, *head_only_html) &&
      got_doc.size() == want_doc.size()) {
    bool ok = true;
    for (size_t i = 0; ok && i < got_doc.size(); ++i) {
      const PackDecision& want = want_doc[i];
      PackDecision expect = want;
      if (want.kind != nullptr &&
          want.kind->decision_point() == DecisionPoint::kDocumentEnd &&
          want.mode == Mode::kEnforce && want.action != Action::kNone) {
        expect = AsNotRewritable(want);
      }
      ok = got_doc[i] == expect;
    }
    if (ok) return true;
  }
  // (c) The head dropped (an element it must edit was already sent) while the
  // end of the document applied: the page is the document-end-only run's (or
  // that page with the appended blocks moved in front of </html>, as in (a)),
  // every document-end decision is the whole-document one, and every head
  // decision that would have acted says not_rewritable. A head decision that
  // would not have acted is unchanged.
  if (document_only_html != nullptr && got_doc.size() == want_doc.size()) {
    bool ok = true;
    bool dropped = false;
    for (size_t i = 0; ok && i < got_doc.size(); ++i) {
      const PackDecision& want = want_doc[i];
      PackDecision expect = want;
      if (want.kind != nullptr &&
          want.kind->decision_point() == DecisionPoint::kHeadEnd &&
          want.mode == Mode::kEnforce && want.action != Action::kNone) {
        expect = AsNotRewritable(want);
        dropped = true;
      }
      ok = got_doc[i] == expect;
    }
    if (ok && dropped) {
      if (same(chunked.html, *document_only_html)) return true;
      for (size_t k = 1;; ++k) {
        std::optional<std::string> moved =
            RelocateBlocks(*document_only_html, k);
        if (!moved.has_value()) break;
        if (same(chunked.html, *moved)) return true;
      }
    }
  }
  bool passthrough = same(chunked.html, std::string(chunked_roundtrip_html)) &&
                     got_doc.size() == want_doc.size();
  bool dropped = false;
  for (size_t i = 0; passthrough && i < got_doc.size(); ++i) {
    const PackDecision& got = got_doc[i];
    const PackDecision& want = want_doc[i];
    if (got == want) {
      // The HTML is the pack-free one, so an enforcing decision that says it
      // changed something is wrong even when it matches the whole-document
      // run.
      if (want.action != Action::kNone && want.mode == Mode::kEnforce) {
        passthrough = false;
      }
      continue;
    }
    if (got.rule_id == want.rule_id && got.action == Action::kNone &&
        got.reason == Reason::kNotRewritable) {
      dropped = true;
    } else {
      passthrough = false;
    }
  }
  if (passthrough && dropped) return true;
  if (why != nullptr) {
    *why = absl::StrCat(
        "html:\n", chunked.html,
        "\ndecisions: ", DescribeDecisions(chunked.decisions),
        "\nwhole-document decisions: ", DescribeDecisions(whole.decisions));
  }
  return false;
}

bool ChunkedOutcomeAllowedForPacks(
    const RunResult& whole, const CaseRun& run, const RunResult& chunked,
    std::string_view chunked_roundtrip_html, std::string* why,
    const std::function<bool(const std::string&, const std::string&)>&
        same_html) {
  for (size_t s = 0; s < run.drop_states.size(); ++s) {
    const std::vector<unsigned>& state = run.drop_states[s];
    RunResult reference = run.drop_runs[s];
    reference.decisions = whole.decisions;
    for (size_t i = 0; i < reference.decisions.size(); ++i) {
      PackDecision& d = reference.decisions[i];
      if (d.kind == nullptr || d.mode != Mode::kEnforce ||
          d.action == Action::kNone || d.header) {
        continue;
      }
      const unsigned dropped = state[whole.decision_pack[i]];
      const DecisionPoint point = d.kind->decision_point();
      if ((point == DecisionPoint::kHeadEnd && (dropped & kHeadDropped) != 0) ||
          (point == DecisionPoint::kDocumentEnd &&
           (dropped & kDocumentDropped) != 0)) {
        d = AsNotRewritable(d);
      }
    }
    if (ChunkedOutcomeAllowed(reference, chunked, chunked_roundtrip_html,
                              nullptr, same_html)) {
      return true;
    }
  }
  if (why != nullptr) {
    *why = absl::StrCat(
        "html:\n", chunked.html,
        "\ndecisions: ", DescribeDecisions(chunked.decisions),
        "\nwhole-document decisions: ", DescribeDecisions(whole.decisions));
  }
  return false;
}

namespace {

// Offset just past the '>' of the first <head> start tag, or nullopt.
std::optional<size_t> AfterHeadStart(std::string_view lower) {
  for (size_t at = lower.find("<head"); at != std::string_view::npos;
       at = lower.find("<head", at + 1)) {
    const size_t next = at + 5;
    if (next >= lower.size()) return std::nullopt;
    const char c = lower[next];
    if (c != '>' && !absl::ascii_isspace(static_cast<unsigned char>(c))) {
      continue;  // <header>, ...
    }
    const size_t close = lower.find('>', next);
    if (close == std::string_view::npos) return std::nullopt;
    return close + 1;
  }
  return std::nullopt;
}

}  // namespace

std::vector<Chunking> ChunkingsFor(std::string_view html) {
  std::vector<Chunking> out;
  for (size_t every : {16, 64, 256}) {
    Chunking c;
    c.name = absl::StrCat("every-", every);
    for (size_t at = every; at < html.size(); at += every) {
      c.flush_after.push_back(at);
    }
    out.push_back(std::move(c));
  }
  const std::string lower = absl::AsciiStrToLower(html);
  if (std::optional<size_t> at = AfterHeadStart(lower); at.has_value()) {
    out.push_back({"after-head", {*at}});
  }
  if (size_t at = lower.find("</head"); at != std::string::npos && at > 0) {
    out.push_back({"before-head-close", {at}});
  }
  return out;
}

absl::StatusOr<CaseRun> RunCase(const std::string& fixtures_dir,
                                const FixtureCase& c) {
  auto req_text = ReadFile(c.dir + "/request.json");
  if (!req_text.ok()) return req_text.status();
  auto input = ReadFile(c.dir + "/input.html");
  if (!input.ok()) return input.status();

  Json::Value req;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream req_stream(*req_text);
  if (!Json::parseFromStream(builder, req_stream, &req, &errors) ||
      !req.isObject() || !req["url"].isString()) {
    return absl::InvalidArgumentError(c.name + ": request.json needs a url");
  }
  CaseRun run;
  run.input_html = *input;
  run.url = req["url"].asString();
  std::optional<PageUrl> page = ParsePageUrl(run.url);
  if (!page.has_value()) {
    return absl::InvalidArgumentError(c.name + ": bad url " + run.url);
  }
  run.host = page->host;
  run.path = page->path + (page->query.empty() ? "" : "?" + page->query);
  const Mode global = req.get("mode", "enforce").asString() == "report"
                          ? Mode::kReport
                          : Mode::kEnforce;
  const bool debug = req.get("debug", false).asBool();
  run.debug = debug;
  run.directive_mode = global;
  const std::string pack_file = req.get("pack", "pack.json").asString();
  std::vector<std::string> pack_files = {pack_file};
  if (req.isMember("packs")) {
    if (req.isMember("pack") || !req["packs"].isArray() ||
        req["packs"].empty()) {
      return absl::InvalidArgumentError(
          c.name + ": packs is a non-empty array and excludes pack");
    }
    pack_files.clear();
    for (const Json::Value& f : req["packs"]) {
      if (!f.isString()) {
        return absl::InvalidArgumentError(c.name + ": packs lists file names");
      }
      pack_files.push_back(f.asString());
    }
  }
  std::vector<size_t> flush_every;
  if (req.isMember("flush_every")) {
    const size_t every = req["flush_every"].asUInt64();
    if (every == 0) {
      return absl::InvalidArgumentError(c.name + ": flush_every must be > 0");
    }
    flush_every = {every};
  }
  std::optional<std::vector<HeaderLine>> origin_headers;
  if (req.isMember("origin_headers")) {
    std::vector<HeaderLine> lines;
    const Json::Value& list = req["origin_headers"];
    if (!list.isArray()) {
      return absl::InvalidArgumentError(
          c.name + ": origin_headers must be an array of [name, value]");
    }
    for (const Json::Value& pair : list) {
      if (!pair.isArray() || pair.size() != 2 || !pair[0].isString() ||
          !pair[1].isString()) {
        return absl::InvalidArgumentError(
            c.name + ": origin_headers must be an array of [name, value]");
      }
      lines.emplace_back(pair[0].asString(), pair[1].asString());
    }
    origin_headers = std::move(lines);
    run.has_origin_headers = true;
    run.origin_headers = *origin_headers;
  }

  run.pack_file = pack_files.front();
  run.pack_files = pack_files;
  // The packs a server would run: a file whose pack id an earlier one has is
  // refused.
  std::vector<std::shared_ptr<const Pack>> packs;
  std::set<std::string> ids;
  for (const std::string& file : pack_files) {
    auto loaded = LoadPackFile(fixtures_dir + "/" + file, "");
    if (!loaded.ok()) return loaded.status();
    auto loaded_pack = std::make_shared<const Pack>(*std::move(loaded));
    const bool fresh = ids.insert(loaded_pack->info.id).second;
    run.refused.push_back(!fresh);
    if (fresh) packs.push_back(std::move(loaded_pack));
  }
  auto pack = packs.front();

  RunOptions opts;
  opts.global_mode = global;
  opts.debug_comments = debug;
  opts.origin_headers = origin_headers;
  if (!flush_every.empty()) {
    for (size_t at = flush_every[0]; at < run.input_html.size();
         at += flush_every[0]) {
      opts.flush_after.push_back(at);
    }
    run.flush_cuts = opts.flush_after;
  }
  RunOptions report = opts;
  report.global_mode = Mode::kReport;
  RunOptions plain = opts;
  plain.with_filter = false;

  auto run_one = [&](const std::string& html, const RunOptions& o,
                     RunResult* out) -> absl::Status {
    auto r = RunPacksOnHtml(packs, run.url, html, o);
    if (!r.ok()) return r.status();
    *out = *std::move(r);
    return absl::OkStatus();
  };
  absl::Status s;
  if (!(s = run_one(run.input_html, opts, &run.enforce)).ok()) return s;
  if (!(s = run_one(run.input_html, opts, &run.again)).ok()) return s;
  if (!(s = run_one(run.input_html, report, &run.report)).ok()) return s;
  if (!(s = run_one(run.input_html, plain, &run.roundtrip)).ok()) return s;
  // The second pass sees what the first one sent: the document it wrote and
  // the headers it left.
  RunOptions again_opts = opts;
  if (origin_headers.has_value())
    again_opts.origin_headers = run.enforce.headers;
  if (!(s = run_one(run.enforce.html, again_opts, &run.idempotent)).ok()) {
    return s;
  }
  if (packs.size() == 1) {
    {
      // The reference for a chunking that drops the head.
      auto document_only = DocumentEndOnlyPack(*pack);
      auto r = RunPackOnHtml(document_only, run.url, run.input_html, opts);
      if (!r.ok()) return r.status();
      run.document_only = *std::move(r);
    }
    {
      // The reference for a chunking that drops the end of the document.
      auto head_only = HeadOnlyPack(*pack);
      auto r = RunPackOnHtml(head_only, run.url, run.input_html, opts);
      if (!r.ok()) return r.status();
      run.head_only = *std::move(r);
    }
  } else {
    // Every way of dropping the head plan and/or the end-of-document plan of
    // each pack (the packs keep their claims).
    const unsigned states = 1u << (2 * packs.size());
    for (unsigned code = 0; code < states; ++code) {
      std::vector<unsigned> state(packs.size());
      std::vector<std::shared_ptr<const Pack>> variant;
      variant.reserve(packs.size());
      for (size_t i = 0; i < packs.size(); ++i) {
        state[i] = (code >> (2 * i)) & 3u;
        variant.push_back(ClaimOnlyPack(*packs[i], state[i]));
      }
      auto r = RunPacksOnHtml(variant, run.url, run.input_html, opts);
      if (!r.ok()) return r.status();
      run.drop_states.push_back(std::move(state));
      run.drop_runs.push_back(*std::move(r));
    }
  }

  for (Chunking& chunking : ChunkingsFor(run.input_html)) {
    ChunkedRun cr;
    cr.chunking = std::move(chunking);
    RunOptions o = opts, r = report, p = plain;
    // A chunking flushes at its own offsets and at those of flush_every, so
    // it never has fewer flush windows than the whole-document run.
    std::vector<size_t> cuts = cr.chunking.flush_after;
    cuts.insert(cuts.end(), opts.flush_after.begin(), opts.flush_after.end());
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    o.flush_after = r.flush_after = p.flush_after = cuts;
    if (!(s = run_one(run.input_html, o, &cr.enforce)).ok()) return s;
    if (!(s = run_one(run.input_html, r, &cr.report)).ok()) return s;
    if (!(s = run_one(run.input_html, p, &cr.roundtrip)).ok()) return s;
    run.chunked.push_back(std::move(cr));
  }
  return run;
}

bool HasModuleOverride(const FixtureCase& c) {
  std::error_code ec;
  return fs::exists(FilePath(c.dir + "/expected.module.html"), ec) ||
         fs::exists(FilePath(c.dir + "/expected.module.decisions.jsonl"), ec);
}

absl::StatusOr<Expected> ReadExpected(const FixtureCase& c) {
  Expected e;
  auto read_pref = [&](const std::string& module, const std::string& shared,
                       std::string* out, bool* used_module) -> absl::Status {
    auto m = ReadFile(c.dir + "/" + module);
    if (m.ok()) {
      *out = *std::move(m);
      *used_module = true;
      return absl::OkStatus();
    }
    auto s = ReadFile(c.dir + "/" + shared);
    if (!s.ok()) return s.status();
    *out = *std::move(s);
    return absl::OkStatus();
  };
  absl::Status s = read_pref("expected.module.html", "expected.html", &e.html,
                             &e.module_html);
  if (!s.ok()) return s;
  s = read_pref("expected.module.decisions.jsonl", "expected.decisions.jsonl",
                &e.decisions, &e.module_decisions);
  if (!s.ok()) return s;
  if (auto headers = ReadFile(c.dir + "/expected.headers"); headers.ok()) {
    e.headers = *std::move(headers);
  }
  return e;
}

namespace {

absl::Status WriteExpectedFiles(const FixtureCase& c, const CaseRun& run,
                                std::string_view html_name,
                                std::string_view decisions_name) {
  auto write = [](const std::string& path, const std::string& body) {
    std::ofstream out(FilePath(path), std::ios::binary | std::ios::trunc);
    out << body;
    return out ? absl::OkStatus()
               : absl::InternalError(absl::StrCat("cannot write ", path));
  };
  absl::Status s = write(absl::StrCat(c.dir, "/", html_name), run.enforce.html);
  if (!s.ok()) return s;
  if (run.has_origin_headers) {
    s = write(c.dir + "/expected.headers", HeadersToText(run.enforce.headers));
    if (!s.ok()) return s;
  }
  return write(
      absl::StrCat(c.dir, "/", decisions_name),
      DecisionsToJsonl(run.host, run.path, run.enforce.decisions, false));
}

}  // namespace

absl::Status WriteModuleExpected(const FixtureCase& c, const CaseRun& run) {
  return WriteExpectedFiles(c, run, "expected.module.html",
                            "expected.module.decisions.jsonl");
}

absl::Status WriteNewCaseExpected(const FixtureCase& c, const CaseRun& run) {
  return WriteExpectedFiles(c, run, "expected.html",
                            "expected.decisions.jsonl");
}

std::string StripPackComments(std::string_view html) {
  static constexpr std::string_view kOpen = "<!-- pagespeed-pack ";
  static constexpr std::string_view kClose = "-->";
  std::string out;
  size_t at = 0;
  while (true) {
    const size_t start = html.find(kOpen, at);
    if (start == std::string_view::npos) break;
    const size_t end = html.find(kClose, start + kOpen.size());
    if (end == std::string_view::npos) break;
    out.append(html.substr(at, start - at));
    at = end + kClose.size();
  }
  out.append(html.substr(at));
  return out;
}

}  // namespace pagespeed::packs

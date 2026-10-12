// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Memory-safety harness for the rule-pack filter.
//
// RulePackFilter reads attacker-controlled HTML as it streams through the
// parser, one flush window at a time, and edits the head in place. Build with
//
//     bazel build --config=clang-asan -c opt //pagespeed/kernel/packs:rule_pack_filter_fuzz
//
// so AddressSanitizer is the oracle.
//
// One source, two build modes (the same split as the loader harness):
//   * default                       -> deterministic main() that replays
//                                      every file named on the command line.
//   * -DRULE_PACK_FILTER_LIBFUZZER  -> exposes LLVMFuzzerTestOneInput for
//                                      coverage-guided discovery when linked
//                                      with -fsanitize=fuzzer.
//
// Input layout: byte 0 picks the page path (and so the rules that apply),
// byte 1 seeds the chunk sizes, the rest is the HTML. Every input is parsed
// with a fixed pack, cut into chunks with a Flush() after each, three times:
// enforcing, report-only, and without the filter. Beyond not crashing, the
// report-only output must be byte-identical to the output without the filter
// (report mode never changes a page).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/html/html_writer_filter.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_host.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/rule_pack_filter.h"

namespace {

using pagespeed::packs::Counter;
using pagespeed::packs::LogLevel;
using pagespeed::packs::Mode;
using pagespeed::packs::Pack;
using pagespeed::packs::PackDecision;
using pagespeed::packs::PackHost;
using pagespeed::packs::PageUrl;
using pagespeed::packs::RulePackFilter;

// Every kind under every rung, a table, a template with captures, and a path
// no rule matches. The hreflang rules list the page itself as the "en" entry;
// the jsonld rules quote the head values the document ends with.
constexpr char kPack[] = R"({
  "pack": {"id": "edge-seo", "version": "0.1.0"},
  "sites": [{"host": "fixture.test", "mode": "enforce"}],
  "tables": {"titles": {"/t/table": "Fish & Chips <b>"}},
  "rules": [
    {"id": "ck", "kind": "canonical", "enforce": true, "on_present": "keep",
     "match": {"paths": ["/k/**"]},
     "value": {"template": "https://fixture.test{path}"}},
    {"id": "cr", "kind": "canonical", "enforce": true, "on_present": "repair",
     "match": {"paths": ["/r/**"]},
     "value": {"template": "https://fixture.test{path}"}},
    {"id": "cx", "kind": "canonical", "enforce": true, "on_present": "replace",
     "match": {"paths": ["/x/**"], "path_regex": "^/x/([a-z]*)(/.*)?$"},
     "value": {"template": "https://fixture.test/{1}?a=1&b=2"}},
    {"id": "tk", "kind": "title", "enforce": true, "on_present": "keep",
     "match": {"paths": ["/k/**"]}, "value": {"template": "T <{path}> &"}},
    {"id": "tr", "kind": "title", "enforce": true, "on_present": "repair",
     "match": {"paths": ["/r/**"]}, "value": {"template": "T {path}"}},
    {"id": "tx", "kind": "title", "enforce": true, "on_present": "replace",
     "match": {"paths": ["/x/**", "/t/**"]},
     "value": {"table": "titles", "fallback_template": "T {path}"}},
    {"id": "dk", "kind": "description", "enforce": true, "on_present": "keep",
     "match": {"paths": ["/k/**"]}, "value": {"template": "D \"{path}\""}},
    {"id": "dr", "kind": "description", "enforce": true,
     "on_present": "repair", "match": {"paths": ["/r/**"]},
     "value": {"template": "D {path}"}},
    {"id": "dx", "kind": "description", "enforce": true,
     "on_present": "replace", "match": {"paths": ["/x/**"]},
     "value": {"template": "D {path}"}},
    {"id": "hk", "kind": "hreflang", "enforce": true, "on_present": "keep",
     "match": {"paths": ["/k/**"]},
     "value": {"pattern": {"en": "https://fixture.test{path}",
                           "de": "https://fixture.test/de{path}",
                           "x-default": "https://fixture.test{path}"}}},
    {"id": "hr", "kind": "hreflang", "enforce": true, "on_present": "repair",
     "match": {"paths": ["/r/**"]},
     "value": {"pattern": {"en": "https://fixture.test{path}",
                           "de": "https://fixture.test/de{path}"}}},
    {"id": "hx", "kind": "hreflang", "enforce": true, "on_present": "replace",
     "match": {"paths": ["/x/**"]},
     "value": {"pattern": {"en": "https://fixture.test{path}",
                           "fr-ca": "https://fixture.test/fr{path}"}}},
    {"id": "jk", "kind": "jsonld", "enforce": true, "on_present": "keep",
     "match": {"paths": ["/k/**"]},
     "value": {"template": "{\"@context\":\"https://schema.org\",\"@type\":\"WebPage\",\"name\":\"{title}\",\"url\":\"{canonical}\"}"}},
    {"id": "jr", "kind": "jsonld", "enforce": true, "on_present": "repair",
     "match": {"paths": ["/r/**"]},
     "value": {"template": "{\"@type\":\"Organization\",\"name\":\"{description}\"}"}},
    {"id": "jx", "kind": "jsonld", "enforce": true, "on_present": "replace",
     "match": {"paths": ["/x/**"]},
     "value": {"template": "{\"@graph\":[{\"@type\":\"WebPage\",\"name\":\"{title}\"},{\"@type\":\"BreadcrumbList\"}]}"}}
  ]
})";

constexpr const char* kUrls[] = {
    "https://fixture.test/k/a",     "https://fixture.test/r/b",
    "https://fixture.test/x/c",     "https://fixture.test/x/d/e?q=1",
    "https://fixture.test/t/table", "https://fixture.test/none",
};

class NullHost : public PackHost {
 public:
  explicit NullHost(bool debug) : debug_(debug) {}
  Headers* response_headers() override { return nullptr; }
  std::optional<std::string> RequestHeader(std::string_view) const override {
    return std::nullopt;
  }
  void Log(LogLevel, std::string_view) override {}
  void Count(Counter, int64_t) override {}
  void Decision(const PageUrl&, const PackDecision&) override {}
  int64_t NowMs() const override { return 0; }
  bool debug_comments() const override { return debug_; }

 private:
  bool debug_;
};

std::shared_ptr<const Pack> FixedPack() {
  static std::shared_ptr<const Pack>* pack = [] {
    auto loaded = pagespeed::packs::LoadPack(kPack, "", "fuzz-pack.json");
    if (!loaded.ok()) {
      std::fprintf(stderr, "fixed pack does not load: %s\n",
                   std::string(loaded.status().message()).c_str());
      std::abort();
    }
    return new std::shared_ptr<const Pack>(
        std::make_shared<const Pack>(*std::move(loaded)));
  }();
  return *pack;
}

// Parses `html` in chunks whose sizes come from `seed`, with a Flush() after
// each, through the filter (unless `mode` is empty) and a writer.
std::string Run(std::string_view url, std::string_view html, uint8_t seed,
                std::optional<Mode> mode, bool debug) {
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  NullHost host(debug);
  std::unique_ptr<RulePackFilter> filter;
  if (mode.has_value()) {
    filter =
        std::make_unique<RulePackFilter>(&parser, FixedPack(), *mode, &host);
    parser.AddFilter(filter.get());
  }
  std::string out;
  net_instaweb::StringWriter writer(&out);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);
  if (!parser.StartParse(url)) return out;
  uint32_t state = seed * 2654435761u + 1;
  size_t at = 0;
  while (at < html.size()) {
    state = state * 1103515245u + 12345u;
    // Mostly small chunks (the interesting boundaries), sometimes large.
    size_t n = 1 + (state >> 16) % ((seed & 1) ? 7 : 97);
    if (((state >> 8) & 15) == 0) n += 512;
    n = std::min(n, html.size() - at);
    parser.ParseText(html.substr(at, n));
    parser.Flush();
    at += n;
  }
  parser.FinishParse();
  return out;
}

// `html` without the filter's debug comments. With debug comments on, report
// mode marks what it would change and must change nothing else. A debug
// comment is one line of text with no markup in it, so a "<!-- pagespeed-pack "
// in the page that is not closed by a "-->" before the next '<' or '>' is page
// content (an unterminated comment, a mangled end tag), not a comment of the
// filter's, and stays: otherwise a later real comment would close it and the
// strip would eat the page text in between on one side only.
std::string StripPackComments(std::string_view html) {
  static constexpr std::string_view kOpen = "<!-- pagespeed-pack ";
  std::string out;
  size_t at = 0;
  size_t from = 0;
  while (true) {
    const size_t start = html.find(kOpen, from);
    if (start == std::string_view::npos) break;
    const size_t body = start + kOpen.size();
    const size_t stop = html.find_first_of("<>", body);
    if (stop == std::string_view::npos || html[stop] != '>' ||
        stop < body + 2 || html[stop - 1] != '-' || html[stop - 2] != '-') {
      from = start + 1;  // page content, not a comment of the filter's
      continue;
    }
    out.append(html.substr(at, start - at));
    at = stop + 1;
    from = at;
  }
  out.append(html.substr(at));
  return out;
}

void RunOne(const uint8_t* data, size_t size) {
  if (size < 2) return;
  const std::string_view url = kUrls[data[0] % std::size(kUrls)];
  const uint8_t seed = data[1];
  const bool debug = (data[0] & 0x80) != 0;
  const std::string_view html(reinterpret_cast<const char*>(data + 2),
                              size - 2);
  Run(url, html, seed, Mode::kEnforce, debug);
  const std::string report = Run(url, html, seed, Mode::kReport, debug);
  const std::string plain = Run(url, html, seed, std::nullopt, debug);
  // Byte-identical without debug comments. With them, the only difference
  // allowed is the would= comments (stripped from both sides, so a page
  // that carries such a comment itself compares equal too).
  if (debug ? StripPackComments(report) != StripPackComments(plain)
            : report != plain) {
    std::fprintf(stderr,
                 "report mode changed the page\n--- without the filter\n%s\n"
                 "--- report mode\n%s\n",
                 plain.c_str(), report.c_str());
    std::abort();
  }
}

}  // namespace

#ifdef RULE_PACK_FILTER_LIBFUZZER
// Coverage-guided entry point. Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  net_instaweb::HtmlKeywords::Init();
  RunOne(data, size);
  return 0;
}
#else
int main(int argc, char** argv) {
  net_instaweb::HtmlKeywords::Init();
  size_t n = 0;
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot open %s\n", argv[i]);
      continue;
    }
    std::string buf;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    RunOne(reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
    ++n;
  }
  std::fprintf(
      stderr, "rule pack filter fuzz harness: %zu inputs processed clean\n", n);
  return n == 0 ? 1 : 0;
}
#endif

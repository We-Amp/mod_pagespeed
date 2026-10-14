// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Per-page cost of RulePackFilter: a 100 KB page parsed and written with and
// without the filter (one per pack, sharing the claims of the page as on a
// server with several packs), many times; prints p50/p99 per page and the
// difference.
//
//   bazel run -c opt //test/pagespeed/kernel/packs:rule_pack_filter_benchmark
//
// Optional argument: the number of iterations (default 2000).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/html/html_writer_filter.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/rule_pack_filter.h"
#include "test/pagespeed/kernel/packs/test_pack_host.h"

namespace pagespeed::packs {
namespace {

constexpr char kExamplePack[] =
    "test/pagespeed/kernel/packs/testdata/edge-seo/pack.example.json";

// Every head kind, enforcing, under replace: the most work per page.
constexpr char kEnforcingPack[] = R"({
  "pack": {"id": "edge-seo", "version": "0.1.0"},
  "sites": [{"host": "www.example.com", "mode": "enforce"}],
  "rules": [
    {"id": "c", "kind": "canonical", "enforce": true, "on_present": "replace",
     "value": {"template": "https://www.example.com{path}"}},
    {"id": "t", "kind": "title", "enforce": true, "on_present": "replace",
     "value": {"template": "Blue widgets | {path}"}},
    {"id": "d", "kind": "description", "enforce": true,
     "on_present": "replace", "value": {"template": "All about {path}"}}
  ]
})";

// kEnforcingPack split in two packs with disjoint kinds: the same work as
// one pack, run by two filters.
constexpr char kCanonicalTitlePack[] = R"({
  "pack": {"id": "edge-seo", "version": "0.1.0"},
  "sites": [{"host": "www.example.com", "mode": "enforce"}],
  "rules": [
    {"id": "c", "kind": "canonical", "enforce": true, "on_present": "replace",
     "value": {"template": "https://www.example.com{path}"}},
    {"id": "t", "kind": "title", "enforce": true, "on_present": "replace",
     "value": {"template": "Blue widgets | {path}"}}
  ]
})";
constexpr char kDescriptionPack[] = R"({
  "pack": {"id": "edge-seo", "version": "0.1.0"},
  "sites": [{"host": "www.example.com", "mode": "enforce"}],
  "rules": [
    {"id": "d", "kind": "description", "enforce": true,
     "on_present": "replace", "value": {"template": "All about {path}"}}
  ]
})";

using Packs = std::vector<std::shared_ptr<const Pack>>;

// `pack` under another id (the loader may accept only one).
std::shared_ptr<const Pack> WithId(const std::shared_ptr<const Pack>& pack,
                                   const char* id) {
  Pack copy = *pack;
  copy.info.id = id;
  return std::make_shared<const Pack>(std::move(copy));
}

// A page shaped like a content-management-system page: a busy head, then
// body markup up to `bytes`.
std::string MakePage(size_t bytes) {
  std::string html =
      "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
      "<meta name=\"viewport\" content=\"width=device-width, "
      "initial-scale=1\">\n<title>Blue widgets &amp; more</title>\n"
      "<meta name=\"description\" content=\"Widgets of every colour\">\n"
      "<link rel=\"canonical\" href=\"https://www.example.com/old\">\n";
  for (int i = 0; i < 12; ++i) {
    absl::StrAppend(&html, "<link rel=\"stylesheet\" href=\"/css/s", i,
                    ".css?ver=6.4\" media=\"all\">\n");
  }
  for (int i = 0; i < 8; ++i) {
    absl::StrAppend(&html, "<script src=\"/js/s", i,
                    ".js?ver=6.4\" defer></script>\n");
  }
  for (int i = 0; i < 6; ++i) {
    absl::StrAppend(&html, "<meta property=\"og:x", i,
                    "\" content=\"value ", i, "\">\n");
  }
  html += "<style>body{margin:0;font-family:sans-serif}</style>\n</head>\n"
          "<body class=\"home page\">\n<header><nav><ul>";
  for (int i = 0; i < 20; ++i) {
    absl::StrAppend(&html, "<li><a href=\"/section/", i, "\">Section ", i,
                    "</a></li>");
  }
  html += "</ul></nav></header>\n<main>\n";
  for (int i = 0; html.size() < bytes - 200; ++i) {
    absl::StrAppend(
        &html, "<article id=\"a", i, "\"><h2>Widget ", i,
        "</h2><p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
        "sed do eiusmod tempor <a href=\"/w/", i,
        "\">incididunt</a> ut labore.</p><img src=\"/img/w", i,
        ".jpg\" alt=\"Widget\" width=\"300\" height=\"200\"></article>\n");
  }
  html += "</main>\n<footer><p>Footer</p></footer>\n</body>\n</html>\n";
  return html;
}

// One parse + write of `html`; returns the time it took in microseconds.
double RunOnce(const Packs& packs, Mode mode, const std::string& url,
               const std::string& html, std::string* out) {
  const auto start = std::chrono::steady_clock::now();
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  ClaimSet claims;
  std::vector<std::unique_ptr<TestPackHost>> hosts;
  std::vector<std::unique_ptr<RulePackFilter>> filters;
  for (const auto& pack : packs) {
    hosts.push_back(std::make_unique<TestPackHost>());
    hosts.back()->set_claims(&claims);
    filters.push_back(std::make_unique<RulePackFilter>(&parser, pack, mode,
                                                       hosts.back().get()));
    parser.AddFilter(filters.back().get());
  }
  out->clear();
  net_instaweb::StringWriter writer(out);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);
  parser.StartParse(url);
  parser.ParseText(html);
  parser.FinishParse();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::micro>(end - start).count();
}

double Percentile(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  const size_t i = static_cast<size_t>(p * (v.size() - 1) + 0.5);
  return v[i];
}

void Measure(const char* label, const Packs& packs, Mode mode,
             const std::string& url, const std::string& html, int iterations) {
  std::string out;
  std::vector<double> with, without;
  for (int i = 0; i < 50; ++i) {  // warm up
    RunOnce(packs, mode, url, html, &out);
    RunOnce({}, mode, url, html, &out);
  }
  for (int i = 0; i < iterations; ++i) {
    with.push_back(RunOnce(packs, mode, url, html, &out));
    without.push_back(RunOnce({}, mode, url, html, &out));
  }
  const double p50w = Percentile(with, 0.5), p99w = Percentile(with, 0.99);
  const double p50n = Percentile(without, 0.5),
               p99n = Percentile(without, 0.99);
  std::printf(
      "%-48s with filter p50 %7.1f us p99 %7.1f us | without p50 %7.1f us "
      "p99 %7.1f us | overhead p50 %+6.1f us p99 %+6.1f us\n",
      label, p50w, p99w, p50n, p99n, p50w - p50n, p99w - p99n);
}

}  // namespace
}  // namespace pagespeed::packs

int main(int argc, char** argv) {
  using pagespeed::packs::LoadPack;
  using pagespeed::packs::LoadPackFile;
  using pagespeed::packs::Mode;
  using pagespeed::packs::Pack;
  net_instaweb::HtmlKeywords::Init();
  const int iterations = argc > 1 ? std::atoi(argv[1]) : 2000;
  auto example = LoadPackFile(pagespeed::packs::kExamplePack, "");
  auto enforcing = LoadPack(pagespeed::packs::kEnforcingPack, "");
  auto canonical_title = LoadPack(pagespeed::packs::kCanonicalTitlePack, "");
  auto description = LoadPack(pagespeed::packs::kDescriptionPack, "");
  if (!example.ok() || !enforcing.ok() || !canonical_title.ok() ||
      !description.ok()) {
    std::fprintf(stderr, "packs do not load\n");
    return 1;
  }
  auto example_pack = std::make_shared<const Pack>(*std::move(example));
  auto enforcing_pack = std::make_shared<const Pack>(*std::move(enforcing));
  auto canonical_title_pack =
      std::make_shared<const Pack>(*std::move(canonical_title));
  auto description_pack = pagespeed::packs::WithId(
      std::make_shared<const Pack>(*std::move(description)), "fixture-second");
  const std::string html = pagespeed::packs::MakePage(100 * 1024);
  std::printf("page: %zu bytes, %d iterations each\n", html.size(),
              iterations);
  pagespeed::packs::Measure("example pack, /shop/blue-widget (report)",
                            {example_pack}, Mode::kEnforce,
                            "https://www.example.com/shop/blue-widget", html,
                            iterations);
  pagespeed::packs::Measure("example pack, / (report)", {example_pack},
                            Mode::kEnforce, "https://www.example.com/", html,
                            iterations);
  pagespeed::packs::Measure("3 enforcing replace rules, /p", {enforcing_pack},
                            Mode::kEnforce, "https://www.example.com/p", html,
                            iterations);
  pagespeed::packs::Measure("host not listed", {example_pack}, Mode::kEnforce,
                            "https://example.com/", html, iterations);
  // Two packs on one host; compare with "3 enforcing replace rules".
  pagespeed::packs::Measure("2 packs, the 3 rules split by kind, /p",
                            {canonical_title_pack, description_pack},
                            Mode::kEnforce, "https://www.example.com/p", html,
                            iterations);
  pagespeed::packs::Measure(
      "2 packs, same kinds (2nd claimed), /p",
      {enforcing_pack,
       pagespeed::packs::WithId(enforcing_pack, "fixture-second")},
      Mode::kEnforce, "https://www.example.com/p", html, iterations);
  return 0;
}

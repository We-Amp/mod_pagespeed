// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/llms_txt_builder.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lib/base/string_util.h"
#include "lib/base/url_util.h"
#include "lib/classify/hostname.h"
#include "lib/html/html_metadata_extractor.h"
#include "lib/html/llms_txt_formatter.h"
#include "lib/html/robots_ai_directives.h"
#include "lib/html/sitemap_parser.h"
#include "sha256.hpp"

namespace pagespeed {

// The pure lib/html helpers live in net_instaweb (mod_pagespeed heritage); this
// builder lives in pagespeed (src/worker convention). Pull the names in.
using net_instaweb::AnalyzeRobotsForAi;
using net_instaweb::ExtractHtmlMetadata;
using net_instaweb::FormatLlmsTxt;
using net_instaweb::HtmlMetadata;
using net_instaweb::LlmsTxtLink;
using net_instaweb::LlmsTxtSection;
using net_instaweb::PageExcludedByAiDirectives;
using net_instaweb::ParsedSitemap;
using net_instaweb::ParseSitemap;
using net_instaweb::StringCaseEqual;
using net_instaweb::TrimHtmlWhitespace;

namespace {

// Extract the path (with query) from an absolute "scheme://host/path?q" URL.
// Returns "/" when the authority has no path. A relative/opaque URL is returned
// unchanged. Any fragment is stripped.
std::string UrlPath(std::string_view url) {
  std::size_t scheme = url.find("://");
  std::string_view rest;
  if (scheme == std::string_view::npos) {
    rest = url;  // relative — treat the whole thing as a path
  } else {
    std::string_view after = url.substr(scheme + 3);
    std::size_t slash = after.find('/');
    if (slash == std::string_view::npos) return "/";
    rest = after.substr(slash);
  }
  std::size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);
  return rest.empty() ? std::string("/") : std::string(rest);
}

// True iff `path` is permitted by the allow-list. Empty list, or any entry that
// is "/" (or "/*"), allows everything. Otherwise the path must be prefixed by an
// allow entry (a trailing "*" is treated as a prefix wildcard).
bool PathAllowed(std::string_view path,
                 const std::vector<std::string>& allow_paths) {
  if (allow_paths.empty()) return true;
  for (const std::string& entry : allow_paths) {
    if (entry.empty()) continue;
    if (entry == "/" || entry == "/*") return true;
    std::string_view prefix = entry;
    if (!prefix.empty() && prefix.back() == '*') prefix.remove_suffix(1);
    if (path.size() >= prefix.size() &&
        path.substr(0, prefix.size()) == prefix) {
      return true;
    }
  }
  return false;
}

// Derive a human-ish title from a URL path's last segment:
// "/docs/getting-started.html" -> "Getting Started"; "/" -> "Home".
std::string TitleFromPath(std::string_view path) {
  // Drop a trailing slash, then take the last segment.
  while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  if (path == "/" || path.empty()) return "Home";
  std::size_t slash = path.find_last_of('/');
  std::string_view seg =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  // Strip a query and a file extension.
  std::size_t q = seg.find('?');
  if (q != std::string_view::npos) seg = seg.substr(0, q);
  std::size_t dot = seg.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) seg = seg.substr(0, dot);
  std::string out;
  bool word_start = true;
  for (char c : seg) {
    if (c == '-' || c == '_' || c == '+' || c == '%') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      word_start = true;
      continue;
    }
    if (word_start && c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
    word_start = false;
  }
  std::string_view trimmed(out);
  TrimHtmlWhitespace(&trimmed);
  return trimmed.empty() ? std::string(seg) : std::string(trimmed);
}

// The grouping section for a path: the first path segment when the page is
// nested (>= 2 segments), else "Pages" for top-level / root pages.
std::string SectionForPath(std::string_view path) {
  std::vector<std::string_view> segs;
  std::size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    std::size_t start = i;
    while (i < path.size() && path[i] != '/') ++i;
    if (i > start) segs.push_back(path.substr(start, i - start));
  }
  if (segs.size() >= 2) return std::string(segs[0]);
  return "Pages";
}

// Pull a title (first "# " heading) + summary (first following non-heading,
// non-empty line) out of already-rendered agent markdown.
void TitleAndSummaryFromMarkdown(std::string_view md, std::string* title,
                                 std::string* summary) {
  std::size_t pos = 0;
  bool have_title = false;
  while (pos < md.size()) {
    std::size_t nl = md.find('\n', pos);
    std::string_view line = md.substr(
        pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = (nl == std::string_view::npos) ? md.size() : nl + 1;
    std::string_view trimmed = line;
    TrimHtmlWhitespace(&trimmed);
    if (trimmed.empty()) continue;
    if (!have_title) {
      if (trimmed.size() >= 2 && trimmed[0] == '#') {
        std::size_t h = 0;
        while (h < trimmed.size() && trimmed[h] == '#') ++h;
        std::string_view t = trimmed.substr(h);
        TrimHtmlWhitespace(&t);
        *title = std::string(t);
        have_title = true;
      }
      continue;
    }
    // First non-empty, non-heading line after the title is the summary.
    if (trimmed[0] == '#') continue;
    *summary = std::string(trimmed);
    return;
  }
}

std::array<std::byte, 32> Sha256(std::string_view bytes) {
  return cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
}

}  // namespace

LlmsTxtBuilder::LlmsTxtBuilder(LlmsTxtFetchFn fetch,
                               LlmsTxtVariantReaderFn variant_reader)
    : fetch_(std::move(fetch)), variant_reader_(std::move(variant_reader)) {}

LlmsTxtBuildResult LlmsTxtBuilder::Build(
    const LlmsTxtBuildOptions& opts) const {
  LlmsTxtBuildResult result;
  const std::string origin = opts.scheme + "://" + opts.host;
  // Default-port-normalized own-origin host (so a sitemap <loc> written as
  // https://host:443/p still matches the bare own host). Both sides normalized.
  const std::string norm_host = NormalizeHostname(opts.host);

  // (1) AI directives: a site-wide robots.txt AI block suppresses synthesis.
  if (opts.respect_ai_directives) {
    LlmsTxtFetchResult robots = fetch_(origin + "/robots.txt");
    if (robots.ok && AnalyzeRobotsForAi(robots.body).ai_blocked_site_wide) {
      result.status = LlmsTxtBuildStatus::kAiBlocked;
      return result;
    }
    // robots unreachable -> permissive (do not suppress).
  }

  // (2) Fetch + hash the sitemap; collect candidate page URLs.
  std::vector<std::string> page_urls;
  LlmsTxtFetchResult sm = fetch_(origin + opts.sitemap_path);
  if (sm.ok) {
    result.sitemap_hash = Sha256(sm.body);
    ParsedSitemap parsed = ParseSitemap(sm.body);
    page_urls = std::move(parsed.page_locs);
    // Bounded one-level fan-out into nested (own-origin) sitemaps.
    std::size_t fetched_nested = 0;
    for (const std::string& nested : parsed.nested_sitemap_locs) {
      if (fetched_nested >= kLlmsTxtMaxNestedSitemaps) break;
      if (!StringCaseEqual(NormalizeHostname(pagespeed::UrlHostname(nested)),
                           norm_host)) {
        continue;
      }
      ++fetched_nested;
      LlmsTxtFetchResult ns = fetch_(nested);
      if (!ns.ok) continue;
      ParsedSitemap p2 = ParseSitemap(ns.body);
      for (std::string& loc : p2.page_locs) page_urls.push_back(std::move(loc));
    }
  } else {
    // (D-OQ3) Fallback when no sitemap is reachable: the concrete (non-wildcard)
    // allow-path entries become the page set. sitemap_hash stays all-zero.
    for (const std::string& p : opts.allow_paths) {
      if (!p.empty() && p[0] == '/' && p.find('*') == std::string::npos) {
        page_urls.push_back(origin + p);
      }
    }
  }

  // (3) Filter to own-origin + allow-listed paths; dedup; cap.
  std::vector<std::string> pages;
  std::unordered_set<std::string> seen;
  for (const std::string& u : page_urls) {
    if (pages.size() >= kLlmsTxtMaxEntries) break;
    if (!StringCaseEqual(NormalizeHostname(pagespeed::UrlHostname(u)),
                         norm_host)) {
      continue;
    }
    if (!PathAllowed(UrlPath(u), opts.allow_paths)) continue;
    if (!seen.insert(u).second) continue;
    pages.push_back(u);
  }
  if (pages.empty()) {
    result.status = LlmsTxtBuildStatus::kNoSource;
    return result;
  }

  // (4) Per-page summary derivation + grouping into sections.
  std::vector<LlmsTxtSection> sections;
  auto section_index = [&](const std::string& name) -> LlmsTxtSection& {
    for (LlmsTxtSection& s : sections) {
      if (s.heading == name) return s;
    }
    sections.push_back(LlmsTxtSection{name, {}});
    return sections.back();
  };

  std::size_t fetch_budget = opts.stub_only ? 0 : opts.summary_fetch_cap;
  std::string site_summary;
  for (const std::string& u : pages) {
    std::string title;
    std::string summary;

    // (a) Free summary from an existing rendered agent-markdown variant.
    if (!opts.stub_only && variant_reader_) {
      std::optional<std::string> md = variant_reader_(u);
      if (md.has_value()) TitleAndSummaryFromMarkdown(*md, &title, &summary);
    }

    // (b) A cheap own-origin GET (NO Chrome) serves BOTH the per-page AI-directive
    // check AND the title/meta summary fallback. When directives are respected we
    // MUST evaluate them per page, so we fetch even when a variant already gave us
    // a summary (the rendered variant carries no directive headers) — bounded by
    // the per-build fetch cap. The stub pass (stub_only) skips all per-page work;
    // it is a transient placeholder, immediately superseded by the verified full
    // build, and the site-wide robots.txt block above still applies to it.
    bool directives_evaluated = !opts.respect_ai_directives || opts.stub_only;
    bool need_fetch =
        !opts.stub_only && (opts.respect_ai_directives || title.empty());
    if (need_fetch && fetch_budget > 0) {
      --fetch_budget;
      LlmsTxtFetchResult pf = fetch_(u);
      if (pf.ok) {
        HtmlMetadata meta = ExtractHtmlMetadata(pf.body, u);
        if (opts.respect_ai_directives &&
            PageExcludedByAiDirectives(pf.x_robots_tag, meta.meta_robots,
                                       pf.google_extended)) {
          continue;  // page opts out of AI use
        }
        directives_evaluated =
            true;  // a 200 without an opt-out header => allowed
        if (title.empty() && !meta.title.empty()) title = meta.title;
        if (summary.empty()) summary = meta.meta_description;
      } else {
        // A failed fetch (network / 4xx / 5xx) is NOT an AI opt-out; the attempt
        // was made, so a sitemap page is not dropped over a transient error.
        directives_evaluated = true;
      }
    }

    // Fail-closed: when directives are respected, never index a page we could not
    // evaluate (e.g. one beyond the fetch cap) — omitting it is safer than leaking
    // a page that may carry noai.
    if (!directives_evaluated) continue;

    // (c) Stub fallback: a path-derived title, no summary.
    if (title.empty()) title = TitleFromPath(UrlPath(u));

    const std::string path = UrlPath(u);
    if (site_summary.empty() && path == "/") site_summary = summary;
    section_index(SectionForPath(path))
        .links.push_back(LlmsTxtLink{title, u, summary});
  }

  // (5) Assemble. Site title = the origin host.
  result.llms_txt = FormatLlmsTxt(opts.host, site_summary, sections);
  result.status = LlmsTxtBuildStatus::kOk;
  return result;
}

}  // namespace pagespeed

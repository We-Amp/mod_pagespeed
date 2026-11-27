// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Analysis Resource Map Implementation

#include "src/worker/analysis_resource_map.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "lib/base/url_util.h"
#include "src/worker/html_css_injector.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

namespace {

// A URL is absolute only when "://" appears before the first "/", "?" or "#":
// a "://" inside a query value ("a.js?next=https://x") must not promote a
// relative src.  (Same intent as ExtractSchemeAndAuthority in
// url_normalizer.cc, with the query/fragment guard added because srcs — unlike
// cache keys — need not start with "/".)
bool IsAbsoluteUrl(std::string_view url) {
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return false;
  size_t delim = url.find_first_of("/?#");
  return delim == std::string_view::npos || scheme_end < delim;
}

// Cache-normalized page URLs are path-only (scheme+authority stripped by
// NormalizeCacheUrl); Chrome joins map keys on absolute request URLs, so all
// resolution runs against this composed absolute form.
std::string AbsolutePageUrl(std::string_view page_url,
                            std::string_view page_hostname,
                            std::string_view scheme) {
  if (IsAbsoluteUrl(page_url)) {
    return std::string(page_url);
  }
  std::string_view s = scheme.empty() ? std::string_view("https") : scheme;
  std::string abs;
  abs.reserve(s.size() + 3 + page_hostname.size() + page_url.size() + 1);
  abs.append(s).append("://").append(page_hostname);
  if (!page_url.starts_with('/')) abs.push_back('/');
  abs.append(page_url);
  return abs;
}

// "https://host/a/b?q" -> "/a/b?q": the cache-key form (ComposeKey prepends
// scheme://hostname itself, so the lookup URL must be path-only).
std::string_view UrlPathAndQuery(std::string_view abs_url) {
  size_t scheme_end = abs_url.find("://");
  if (scheme_end == std::string_view::npos) return abs_url;
  size_t path_start = abs_url.find('/', scheme_end + 3);
  if (path_start == std::string_view::npos) return "/";
  return abs_url.substr(path_start);
}

// "https://host/a/b" -> "https://host" (no trailing slash).
std::string_view UrlOrigin(std::string_view abs_url) {
  size_t scheme_end = abs_url.find("://");
  if (scheme_end == std::string_view::npos) return abs_url;
  size_t path_start = abs_url.find('/', scheme_end + 3);
  if (path_start == std::string_view::npos) return abs_url;
  return abs_url.substr(0, path_start);
}

// Chrome lowercases the host when issuing a request, so a map key must carry
// the lowercased host or a case-variant authored src can never match at
// Fetch-interception time.
void LowercaseUrlHost(std::string& abs_url) {
  size_t scheme_end = abs_url.find("://");
  if (scheme_end == std::string::npos) return;
  size_t host_start = scheme_end + 3;
  size_t host_end = abs_url.find_first_of("/?#", host_start);
  if (host_end == std::string::npos) host_end = abs_url.size();
  for (size_t i = host_start; i < host_end; ++i) {
    abs_url[i] = absl::ascii_tolower(abs_url[i]);
  }
}

// Resolve `ref` against the absolute URL `abs_base` (RFC 3986 subset:
// absolute / protocol-relative / root-relative / path-relative).  The
// protocol-relative branch inherits the base's scheme.
std::string ResolveUrl(std::string_view abs_base, std::string_view ref) {
  if (IsAbsoluteUrl(ref)) return std::string(ref);
  if (ref.starts_with("//")) {
    std::string_view base_scheme = abs_base.substr(0, abs_base.find("://"));
    return absl::StrCat(base_scheme, ":", ref);
  }
  if (ref.starts_with("/")) {
    return absl::StrCat(UrlOrigin(abs_base), ref);
  }
  // Path-relative.  Resolve only the path portion — ResolvePath returns any
  // "://"-carrying input as-is, so a query like "?u=https://x" must never
  // reach it — and re-attach the query/fragment suffix afterwards.
  size_t suffix_at = ref.find_first_of("?#");
  std::string_view path =
      suffix_at == std::string_view::npos ? ref : ref.substr(0, suffix_at);
  std::string_view suffix =
      suffix_at == std::string_view::npos ? "" : ref.substr(suffix_at);
  if (path.empty()) {
    // Query/fragment-only reference: same document, new suffix.
    std::string_view doc = abs_base.substr(0, abs_base.find_first_of("?#"));
    return absl::StrCat(doc, suffix);
  }
  return absl::StrCat(ResolvePath(UrlDirectory(abs_base), path), suffix);
}

}  // namespace

absl::flat_hash_map<std::string, std::string> BuildAnalysisResourceMap(
    std::string_view html, std::string_view page_url,
    std::string_view page_hostname, std::string_view scheme,
    const ResourceLookupFn& lookup, const UrlNormalizationConfig& url_norm,
    AnalysisResourceMapStats* stats) {
  absl::flat_hash_map<std::string, std::string> map;

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(page_url, html);
  if (!scan.success || scan.script_srcs.empty()) return map;
  if (stats != nullptr) stats->scripts_found = scan.script_srcs.size();

  std::string abs_page = AbsolutePageUrl(page_url, page_hostname, scheme);
  // Effective base for relative srcs: the author <base href> when present
  // (itself resolved against the page URL first — the href may be relative),
  // else the page URL, which matches the injected <base>.  This mirrors how
  // Chrome resolves the DOM .src values the map keys must join on.
  std::string abs_base = abs_page;
  if (scan.has_base_href && !scan.base_href.empty()) {
    abs_base = ResolveUrl(abs_page, scan.base_href);
  }
  size_t total_bytes = 0;
  // Unique resolved URLs, hits and misses alike: duplicate srcs must not
  // double-count any stat (the uncached TTL-heal trigger and the
  // cross-origin count alike), so the dedup runs before classification.
  absl::flat_hash_set<std::string> seen;

  for (const std::string& src : scan.script_srcs) {
    if (src.empty() || src.starts_with("data:") ||
        src.starts_with("javascript:")) {
      continue;
    }

    // Map key: the absolute URL Chrome will request (the DOM resolves src
    // against the effective base), in request form — lowercased host,
    // fragment stripped (Chrome strips fragments from requests).
    std::string resolved = ResolveUrl(abs_base, src);
    size_t frag = resolved.find('#');
    if (frag != std::string::npos) resolved.erase(frag);
    LowercaseUrlHost(resolved);

    if (!seen.insert(resolved).second) continue;

    // NOTE: an explicit default port (":443") makes UrlHostname() differ from
    // the page hostname and classifies as cross-origin — a safe miss
    // (blocked -> keep-sync), accepted.
    std::string_view host = UrlHostname(resolved);
    if (!host.empty() && !absl::EqualsIgnoreCase(host, page_hostname)) {
      if (stats != nullptr) ++stats->scripts_cross_origin;
      continue;
    }

    // Look up under the worker's store-key normalization; the raw request
    // form stays the map key.
    auto content =
        lookup(NormalizeCacheUrl(UrlPathAndQuery(resolved), url_norm));
    if (!content.has_value()) {
      // Cache-cold same-origin script: unmapped (blocked -> keep-sync) but
      // healable — once it is fetched through the proxy and cached, a
      // re-analysis can observe it.  This is the TTL-heal trigger.
      if (stats != nullptr) ++stats->scripts_uncached_same_origin;
      continue;
    }
    if (content->empty()) {
      // Cached and empty: nothing to execute, nothing to map (the script
      // still classifies kNoCoverageData -> keep-sync downstream).  A
      // re-analysis re-reads the same zero bytes, so unlike a cache-cold
      // miss this must NOT trip the TTL-heal trigger — an empty stub would
      // otherwise pin hourly re-analysis forever.  Accepted residual (same
      // as the over-cap class): a stub that later gains content heals at
      // the CONFIGURED TTL expiry rather than the 1-hour clamp — healing
      // is delayed, not stopped.
      if (stats != nullptr) ++stats->scripts_empty_same_origin;
      continue;
    }
    if (content->size() > kMaxMappedScriptBytes ||
        total_bytes + content->size() > kMaxMappedTotalBytes) {
      // Over-cap degrades like a miss (unmapped -> blocked -> keep-sync) but
      // can never heal — a re-analysis re-reads the same oversize bytes — so
      // it must not shorten the profile TTL.
      if (stats != nullptr) ++stats->scripts_oversize;
      continue;
    }

    total_bytes += content->size();
    if (stats != nullptr) {
      ++stats->scripts_cached;
      stats->bytes_mapped += content->size();
    }
    map[std::move(resolved)] = std::move(*content);
  }
  return map;
}

std::string InjectBaseHrefIfAbsent(std::string_view html,
                                   std::string_view page_url,
                                   std::string_view page_hostname,
                                   std::string_view scheme) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(page_url, html);
  // Author <base href> wins; on scan failure injection is skipped (relative
  // srcs then stay unresolvable — the prior status quo, never a divergence).
  if (!scan.success || scan.has_base_href) return std::string(html);

  std::string href = AbsolutePageUrl(page_url, page_hostname, scheme);
  std::string tag;
  tag.reserve(href.size() + 16);
  tag.append("<base href=\"");
  for (char c : href) {
    switch (c) {
      case '&':
        tag.append("&amp;");
        break;
      case '"':
        tag.append("&quot;");
        break;
      case '<':
        tag.append("&lt;");
        break;
      case '>':
        tag.append("&gt;");
        break;
      default:
        tag.push_back(c);
    }
  }
  tag.append("\">");

  // Insert immediately after the opening <head ...> tag so the base precedes
  // every script; without a <head> tag, prepend (the HTML parser hoists a
  // pre-<html> base into the synthesized head).  The "head" name must end at
  // a tag boundary — "<header" is not "<head" — and comments are skipped so
  // a "<head>" inside "<!-- -->" is never taken for the real element.
  size_t insert_at = 0;
  for (size_t i = 0; i + 5 <= html.size(); ++i) {
    if (html[i] != '<') continue;
    if (html_scan::StartsComment(html, i)) {
      size_t comment_end = html_scan::ScanComment(html, i);
      // Unterminated comment swallows the rest of the input; fall back to
      // prepending.
      if (comment_end == std::string_view::npos) break;
      i = comment_end - 1;  // Loop increment steps past the comment.
      continue;
    }
    if (!absl::EqualsIgnoreCase(html.substr(i + 1, 4), "head")) continue;
    size_t after = i + 5;
    if (after >= html.size()) break;
    char b = html[after];
    if (b != '>' && b != ' ' && b != '\t' && b != '\n' && b != '\r' &&
        b != '/') {
      continue;
    }
    size_t close = html.find('>', after);
    if (close != std::string_view::npos) insert_at = close + 1;
    break;
  }

  std::string out;
  out.reserve(html.size() + tag.size());
  out.append(html.substr(0, insert_at));
  out.append(tag);
  out.append(html.substr(insert_at));
  return out;
}

}  // namespace pagespeed

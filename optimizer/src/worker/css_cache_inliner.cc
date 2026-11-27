// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CSS Cache Inliner Implementation

#include "src/worker/css_cache_inliner.h"

#include <string>
#include <string_view>
#include <unordered_set>

#include "lib/base/string_util.h"
#include "lib/base/string_writer.h"
#include "lib/base/url_util.h"
#include "lib/css/css_import_flattener.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_name.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

using net_instaweb::LowerChar;

namespace {

// Limits.
constexpr size_t kMaxInlinedStylesheets = 50;
constexpr size_t kMaxInlinedCssSize = 2UL * 1024 * 1024;     // 2MB per sheet
constexpr size_t kMaxEnrichedHtmlSize = 10UL * 1024 * 1024;  // 10MB total

// --- XSS helpers (duplicated from html_css_injector.cc anonymous ns) ---

bool CaseInsensitiveEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerChar(a[i]) != LowerChar(b[i])) {
      return false;
    }
  }
  return true;
}

std::string SanitizeCss(std::string_view css) {
  std::string sanitized;
  sanitized.reserve(css.size());
  for (char c : css) {
    if (c != '\0') sanitized.push_back(c);
  }
  return sanitized;
}

// Returns true if the media attribute value is safe for embedding in
// @media <value> { ... }.  Rejects characters that enable CSS injection
// (braces, semicolons, backslash, @), HTML injection (angle brackets),
// or string breakout (quotes).
bool IsSafeMediaValue(std::string_view media) {
  for (char c : media) {
    if (c == '{' || c == '}' || c == ';' || c == '<' || c == '>' || c == '\\' ||
        c == '\'' || c == '"' || c == '@') {
      return false;
    }
  }
  return true;
}

bool ContainsStyleClose(std::string_view css) {
  for (size_t i = 0; i + 7 <= css.size(); ++i) {
    if (CaseInsensitiveEqual(std::string_view(css.data() + i, 7), "</style")) {
      return true;
    }
  }
  return false;
}

// Filter that deletes the external <link rel="stylesheet"> for every sheet we
// inlined, so the browser loads each logical sheet once instead of fetching the
// external sheet AND parsing the inlined <style> copy (the prod critical-CSS
// double-ship).  The predicate mirrors HtmlScanner's collection gate EXACTLY:
// the same case-sensitive rel == "stylesheet" (or the async-deferred
// rel == "preload" primary the scanner also collects), the same
// data-pagespeed-async-fallback skip, and the same parser-decoded href.  The
// preload arm bites only if this ever runs over a reprocessed variant rather
// than raw origin bytes -- but the two gates silently diverging is exactly the
// failure this comment asserts cannot happen.  Because
// it is driven by the real HtmlParse lexer it removes precisely the links that
// were inlined and never a look-alike the scanner ignored — links inside
// comments or literal-text bodies (<script>/<style>/<iframe>/<xmp>/...), case
// variants (rel="STYLESHEET"), or the worker's own async fallback — which a raw
// byte scan cannot reliably distinguish.
class StylesheetLinkRemover : public net_instaweb::EmptyHtmlFilter {
 public:
  StylesheetLinkRemover(net_instaweb::HtmlParse* parse,
                        const std::unordered_set<std::string_view>& hrefs)
      : parse_(parse), hrefs_(hrefs) {}

  [[nodiscard]] const char* Name() const override {
    return "StylesheetLinkRemover";
  }

  void StartElement(net_instaweb::HtmlElement* element) override {
    if (element->keyword() != net_instaweb::HtmlName::kLink) return;
    const char* rel = element->AttributeValue(net_instaweb::HtmlName::kRel);
    // Case-sensitive, matching ElementCollectorFilter in html_scanner.cc.
    if (rel == nullptr) return;
    const bool is_stylesheet =
        std::string_view(rel) == "stylesheet" ||
        (std::string_view(rel) == "preload" &&
         element->FindAttribute("data-pagespeed-async") != nullptr);
    if (!is_stylesheet) return;
    // The scanner never collects (so never inlines) its own async-CSS fallback.
    if (element->FindAttribute("data-pagespeed-async-fallback") != nullptr) {
      return;
    }
    // AttributeValue() is entity-decoded, matching the decoded hrefs set.
    const char* href = element->AttributeValue(net_instaweb::HtmlName::kHref);
    if (href != nullptr && hrefs_.contains(std::string_view(href))) {
      parse_->DeleteNode(element);
    }
  }

 private:
  net_instaweb::HtmlParse* parse_;
  const std::unordered_set<std::string_view>& hrefs_;
};

// Re-parse `html` with the same lexer the scanner used, delete the inlined
// stylesheet <link>s, and re-serialize.  The output is a NORMALIZED
// re-serialization (inter-attribute whitespace and void-tag brief-close may
// change), not byte-identical to the input — acceptable because these bytes feed
// only the offline analysis Chrome (Page.setDocumentContent), never the served
// response, and the same HtmlParse + HtmlWriterFilter round-trip already
// re-serializes production HTML in the serving path (html_transform_filter).
// On any parse failure return the original bytes (the caller must never produce
// links-stripped HTML without its inlined styles).
std::string RemoveInlinedStylesheetLinks(
    std::string_view html, std::string_view page_url,
    const std::unordered_set<std::string_view>& hrefs) {
  std::string output;
  net_instaweb::StringWriter writer(&output);
  net_instaweb::NullMessageHandler message_handler;
  net_instaweb::HtmlParse parse(&message_handler);
  StylesheetLinkRemover remover(&parse, hrefs);
  net_instaweb::HtmlWriterFilter writer_filter(&parse);
  writer_filter.set_writer(&writer);
  parse.AddFilter(&remover);
  parse.AddFilter(&writer_filter);
  if (!parse.StartParse(page_url)) {
    return std::string(html);
  }
  parse.ParseText(html);
  parse.FinishParse();
  return output;
}

}  // namespace

std::string InlineCachedStylesheets(std::string_view html,
                                    std::string_view page_url,
                                    const css::CssLookupFn& lookup,
                                    CssInliningStats* stats,
                                    std::string_view page_host,
                                    std::string_view page_scheme) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(page_url, html);
  if (!scan.success || scan.stylesheets.empty()) {
    return std::string(html);
  }

  if (stats != nullptr) stats->stylesheets_found = scan.stylesheets.size();

  // What each href resolves against: the page URL, or its <base href> when it
  // has one, the same rule as the serve path's combined-stylesheet gather
  // (DocumentBaseOf), so the coverage render inlines the sheet
  // the browser fetches.
  const DocumentBase document_base =
      DocumentBaseOf(page_url, scan, page_host, page_scheme);
  std::string collected_styles;
  size_t cached_count = 0;
  size_t bytes_inlined = 0;
  // Parser-decoded hrefs (HtmlScanner stores AttributeValue()) of the sheets we
  // inline, so the matching external <link> can be removed afterwards.  Views
  // are stable: they point into scan.stylesheets, which outlives this function.
  std::unordered_set<std::string_view> inlined_hrefs;

  size_t limit = std::min(scan.stylesheets.size(), kMaxInlinedStylesheets);
  for (size_t i = 0; i < limit; ++i) {
    const auto& link = scan.stylesheets[i];

    // Skip empty hrefs (an empty key would later match every valueless-href
    // <link> in RemoveInlinedStylesheetLinks) and data:/javascript: URIs.
    if (link.href.empty()) continue;
    if (link.href.starts_with("data:") || link.href.starts_with("javascript:"))
      continue;

    // Resolve against the document base; a hostless result is the page's own
    // path and the lookup keys it by the page host.
    std::string resolved = ResolveAgainstBase(document_base, link.href);

    // Look up in cache.
    auto content = lookup(resolved);
    if (!content.has_value()) continue;
    if (content->empty()) continue;
    if (content->size() > kMaxInlinedCssSize) continue;

    // Flatten @import chains.  On an all-or-nothing skip, flat.css is
    // the input byte-identical (imports intact, in valid prelude
    // position), so no explicit gate is needed here — this feeds only
    // the offline analysis browser, never served bytes.
    auto flat = css::FlattenImports(*content, resolved, lookup);

    // XSS sanitize: strip null bytes.
    std::string sanitized = SanitizeCss(flat.css);

    // Wrap in @media if media attribute is non-empty and not "all".
    // Validate the media value to prevent CSS injection via crafted
    // media attributes (e.g., media='all) { malicious } @media (all').
    if (!link.media.empty() && !CaseInsensitiveEqual(link.media, "all") &&
        IsSafeMediaValue(link.media)) {
      std::string wrapped;
      wrapped.reserve(sanitized.size() + link.media.size() + 16);
      wrapped.append("@media ");
      wrapped.append(link.media);
      wrapped.append(" { ");
      wrapped.append(sanitized);
      wrapped.append(" }");
      sanitized = std::move(wrapped);
    }

    // Reject if the final CSS (including @media wrapper) contains </style.
    if (ContainsStyleClose(sanitized)) continue;

    collected_styles.append("<style data-pagespeed-inlined>");
    collected_styles.append(sanitized);
    collected_styles.append("</style>\n");
    inlined_hrefs.insert(link.href);
    ++cached_count;
    bytes_inlined += sanitized.size();
  }

  if (stats != nullptr) {
    stats->stylesheets_cached = cached_count;
    stats->bytes_inlined = bytes_inlined;
  }

  if (collected_styles.empty()) return std::string(html);

  // Strip the external <link>s we inlined so the browser loads each sheet once
  // (see RemoveInlinedStylesheetLinks).  On any fall-back below we return the
  // ORIGINAL html unchanged — never a links-stripped page without its styles.
  std::string stripped =
      RemoveInlinedStylesheetLinks(html, page_url, inlined_hrefs);

  // Size cap: enriched HTML must not exceed 10MB.
  if (stripped.size() + collected_styles.size() > kMaxEnrichedHtmlSize) {
    return std::string(html);
  }

  // Find </head> insertion point (case-insensitive).
  for (size_t i = 0; i + 7 <= stripped.size(); ++i) {
    if (CaseInsensitiveEqual(std::string_view(stripped.data() + i, 7),
                             "</head>")) {
      std::string result;
      result.reserve(stripped.size() + collected_styles.size());
      result.append(stripped, 0, i);
      result.append(collected_styles);
      result.append(stripped, i, std::string::npos);
      return result;
    }
  }

  // No </head> found: append at end.
  std::string result;
  result.reserve(stripped.size() + collected_styles.size());
  result.append(stripped);
  result.append(collected_styles);
  return result;
}

}  // namespace pagespeed

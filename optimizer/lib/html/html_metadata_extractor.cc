// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/html_metadata_extractor.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "lib/base/string_util.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"
#include "lib/html/safe_entity_decode.h"

namespace net_instaweb {

namespace {

// Per-field self-bound: a hostile page must not blow up memory via a giant
// <title>. The builder only needs a one-line summary anyway.
constexpr std::size_t kMaxFieldBytes = 2048;

// SAX filter that captures <title> text and <meta name=description|robots>.
// It reads only <head>-level metadata; everything else is ignored.
class MetadataExtractorFilter : public EmptyHtmlFilter {
 public:
  const char* Name() const override { return "MetadataExtractor"; }

  void StartElement(HtmlElement* element) override {
    switch (element->keyword()) {
      case HtmlName::kTitle:
        // Capture the first <title> only.
        if (title_.empty() && !title_done_) in_title_ = true;
        break;
      case HtmlName::kMeta: {
        const char* name = element->AttributeValue(HtmlName::kName);
        if (name == nullptr) break;
        std::string_view nv(name);
        // Read the ESCAPED content (not the decoded value): the decoded path
        // (HtmlKeywords::Unescape) returns EMPTY for any byte > 127, so a
        // description with an em-dash / curly quote / accent was silently
        // dropped. The raw escaped value is decoded UTF-8-safely in
        // ExtractHtmlMetadata below.
        const char* content =
            element->EscapedAttributeValue(HtmlName::kContent);
        if (content == nullptr) break;
        if (StringCaseEqual(nv, "description") && description_.empty()) {
          AssignCapped(&description_, content);
        } else if ((StringCaseEqual(nv, "robots") ||
                    StringCaseEqual(nv, "ai")) &&
                   robots_.empty()) {
          AssignCapped(&robots_, content);
        }
        break;
      }
      case HtmlName::kLink: {
        // <link rel="canonical" href="..."> -> citation-ready canonical URL.
        const char* rel = element->AttributeValue(HtmlName::kRel);
        if (rel == nullptr || !StringCaseEqual(rel, "canonical")) break;
        // Escaped href, decoded UTF-8-safely below (same non-ASCII trap).
        const char* href = element->EscapedAttributeValue(HtmlName::kHref);
        if (href != nullptr && canonical_.empty())
          AssignCapped(&canonical_, href);
        break;
      }
      default:
        break;
    }
  }

  void EndElement(HtmlElement* element) override {
    if (element->keyword() == HtmlName::kTitle && in_title_) {
      in_title_ = false;
      title_done_ = true;
    }
  }

  void Characters(HtmlCharactersNode* characters) override {
    if (!in_title_) return;
    const std::string& c = characters->contents();
    std::size_t room = kMaxFieldBytes - title_.size();
    if (room == 0) return;
    title_.append(c, 0, room < c.size() ? room : c.size());
  }

  std::string title_;
  std::string description_;
  std::string robots_;
  std::string canonical_;

 private:
  static void AssignCapped(std::string* dst, const char* src) {
    std::string_view sv(src);
    *dst = std::string(sv.substr(0, kMaxFieldBytes));
  }

  bool in_title_ = false;
  bool title_done_ = false;
};

}  // namespace

HtmlMetadata ExtractHtmlMetadata(std::string_view html, std::string_view url) {
  HtmlMetadata meta;
  MetadataExtractorFilter filter;
  NullMessageHandler message_handler;
  HtmlParse parser(&message_handler);  // ctor auto-inits HtmlKeywords
  parser.AddFilter(&filter);
  const std::string parse_url =
      url.empty() ? std::string("http://localhost/") : std::string(url);
  if (!parser.StartParse(parse_url)) return meta;
  parser.ParseText(html);
  parser.FinishParse();

  // Decode entities UTF-8-safely (title/description/robots are captured as raw
  // escaped bytes so a non-ASCII glyph is never erased — see the meta read
  // above), THEN collapse surrounding whitespace (<title> often spans lines /
  // has indentation). Decode-before-trim so a decoded &nbsp; (folded to space)
  // is trimmed too.
  std::string title = SafeDecodeHtmlEntities(
      filter.title_,
      /*value_was_capped=*/filter.title_.size() >= kMaxFieldBytes);
  std::string_view t(title);
  TrimHtmlWhitespace(&t);
  meta.title = std::string(t);
  std::string description = SafeDecodeHtmlEntities(
      filter.description_,
      /*value_was_capped=*/filter.description_.size() >= kMaxFieldBytes);
  std::string_view d(description);
  TrimHtmlWhitespace(&d);
  meta.meta_description = std::string(d);
  std::string robots = SafeDecodeHtmlEntities(
      filter.robots_,
      /*value_was_capped=*/filter.robots_.size() >= kMaxFieldBytes);
  std::string_view r(robots);
  TrimHtmlWhitespace(&r);
  meta.meta_robots = std::string(r);
  std::string canonical = SafeDecodeHtmlEntities(
      filter.canonical_,
      /*value_was_capped=*/filter.canonical_.size() >= kMaxFieldBytes);
  std::string_view cn(canonical);
  TrimHtmlWhitespace(&cn);
  meta.canonical = std::string(cn);
  return meta;
}

}  // namespace net_instaweb

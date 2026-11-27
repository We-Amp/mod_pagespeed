// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Scanner Implementation

#include "src/worker/html_scanner.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "lib/base/string_util.h"
#include "lib/base/url_util.h"
#include "lib/classify/url_normalizer.h"
#include "lib/css/css_import_flattener.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"

namespace pagespeed {

namespace {

// True when the inline handler text sets the element's media: an assignment
// to a `media` property (`this.media='all'`, `this['media'] = x`) or a
// `setAttribute('media', ...)` or `removeAttribute('media')` call. Reading
// it, comparing it or any other handler does not.
bool HandlerSetsMedia(const char* handler) {
  if (handler == nullptr) return false;
  const std::string_view text(handler);
  const auto is_ident = [](char c) {
    return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '$';
  };
  const auto is_skipped = [](char c) {
    return absl::ascii_isspace(static_cast<unsigned char>(c)) || c == '\'' ||
           c == '"';
  };
  constexpr std::string_view kMedia = "media";
  for (size_t pos = text.find(kMedia); pos != std::string_view::npos;
       pos = text.find(kMedia, pos + kMedia.size())) {
    if (pos > 0 && is_ident(text[pos - 1])) continue;
    size_t after = pos + kMedia.size();
    if (after < text.size() && is_ident(text[after])) continue;
    // `media = value`, `['media'] = value`; not `media == value`.
    size_t i = after;
    while (i < text.size() && (is_skipped(text[i]) || text[i] == ']')) ++i;
    if (i < text.size() && text[i] == '=' &&
        (i + 1 >= text.size() || text[i + 1] != '=')) {
      return true;
    }
    // `setAttribute('media', value)`, `removeAttribute('media')`.
    size_t j = pos;
    while (j > 0 && is_skipped(text[j - 1])) --j;
    if (j > 0 && text[j - 1] == '(') {
      --j;
      while (j > 0 &&
             absl::ascii_isspace(static_cast<unsigned char>(text[j - 1]))) {
        --j;
      }
      const std::string_view callee = text.substr(0, j);
      if (callee.ends_with("setAttribute") ||
          callee.ends_with("removeAttribute")) {
        return true;
      }
    }
  }
  return false;
}

// Extract the origin (scheme + host) from a URL string.
// Returns empty string if the URL is not absolute (no scheme://host).
std::string ExtractOrigin(std::string_view url) {
  // Must start with http:// or https:// or //
  size_t scheme_end;
  if (url.starts_with("https://")) {
    scheme_end = 8;
  } else if (url.starts_with("http://")) {
    scheme_end = 7;
  } else if (url.starts_with("//")) {
    scheme_end = 2;
  } else {
    return "";
  }
  // Find the end of the host (next / or end of string).
  size_t host_end = url.find('/', scheme_end);
  if (host_end == std::string_view::npos) {
    return std::string(url);
  }
  return std::string(url.substr(0, host_end));
}

// Extract the origin from a page URL for same-origin comparison.
std::string ExtractPageOrigin(std::string_view page_url) {
  return ExtractOrigin(page_url);
}

// The directory a relative reference resolves against: the URL up to its
// last '/', or the URL plus '/' when it has no path ("http://host").
std::string DirectoryOf(std::string_view url) {
  size_t scheme_end = url.find("://");
  if (scheme_end != std::string_view::npos &&
      url.find('/', scheme_end + 3) == std::string_view::npos) {
    return absl::StrCat(url, "/");
  }
  return std::string(UrlDirectory(url));
}

// The scheme of an absolute URL ("http" of "http://host/p"), else empty.
std::string_view SchemeOf(std::string_view url) {
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return {};
  return url.substr(0, scheme_end);
}

// `host` without an explicit default port for `scheme`, for deciding whether
// a <base href> names the page's own host. Identities compare the port
// literally (SheetIdentity); this is only about where a fetch goes.
std::string_view HostWithoutDefaultPort(std::string_view host,
                                        std::string_view scheme) {
  if (scheme == "https" && host.ends_with(":443")) {
    return host.substr(0, host.size() - 4);
  }
  if (scheme == "http" && host.ends_with(":80")) {
    return host.substr(0, host.size() - 3);
  }
  return host;
}

// Priority bucket for preconnect origins: lower number = higher priority.
// 0 = render-blocking (stylesheets, sync scripts)
// 1 = other head resources (preload, etc.)
// 2 = body resources (images, async scripts)
constexpr int kPriorityRenderBlocking = 0;
constexpr int kPriorityHeadResource = 1;
constexpr int kPriorityBodyResource = 2;

// Case-insensitive whole-value match for simple attribute values.
bool AttrValueEquals(const char* value, std::string_view expected) {
  return value != nullptr && net_instaweb::StringCaseEqual(value, expected);
}

// True when the element's direct parent is a <picture> element.  <picture>
// is not an interned HtmlName keyword, so compare the tag name string.
bool ParentIsPicture(const net_instaweb::HtmlElement* element) {
  const net_instaweb::HtmlElement* parent = element->parent();
  return parent != nullptr &&
         net_instaweb::StringCaseEqual(parent->name_str(), "picture");
}

// Where an element sits relative to the subtrees whose stylesheets do not
// apply to a client running scripts. The lexer parses the
// contents of <noscript>, <noembed> and <noframes> as markup (see
// kSometimesLiteralTags in html_lexer.cc), so a <style> or <link> in there
// reaches the scanner as an element even though a browser with scripting on
// treats <noscript> content as raw text, and current browsers do the same for
// the other two. <template> content is inert. HtmlTransformFilter's
// InsideNonDocumentSubtree and the validator name the same subtrees for block
// placement, and also never anchor anywhere inside <svg> or <math>.
//
// The parser reads inline SVG and MathML as browsers do and gives their
// elements no keyword, so an SVG or MathML <style>, <script>, <link> or
// <noscript> never reaches the keyword rules here: its content is markup, not
// the page's CSS. An HTML element inside an integration point (such as SVG
// foreignObject) keeps its keyword and is an HTML element again.
struct StyleScope {
  bool noscript = false;  // inside an HTML <noscript>
  bool inert = false;     // inside an HTML <template>, <noembed> or <noframes>
  bool svg = false;       // an element of SVG content (see ForeignNsOf)
  bool math = false;      // an element of MathML content (see ForeignNsOf)

  // A <link> here is an HTML stylesheet link that applies to the document.
  // In foreign content a <link> is not an HTML element at all.
  [[nodiscard]] bool LinkApplies() const {
    return !noscript && !inert && !svg && !math;
  }
  // A <style> here applies to the document. A <style> with a keyword is an
  // HTML style element, which applies wherever it stands, inside an
  // integration point too.
  [[nodiscard]] bool StyleApplies() const { return !noscript && !inert; }
  // The only place an author's <noscript> twin of a script-loaded sheet can
  // stand: in <noscript> and nowhere else that would disqualify it.
  [[nodiscard]] bool NoscriptOnly() const {
    return noscript && !inert && !svg && !math;
  }
};

enum class ElementNs : std::uint8_t { kHtml, kSvg, kMath };

bool NameIs(const net_instaweb::HtmlElement& element, std::string_view name) {
  return net_instaweb::StringCaseEqual(element.name_str(), name);
}

// The namespace of element when the parser opened it with the rules for
// foreign content, and kHtml when it opened it with the HTML rules (an HTML
// element, or the <svg> or <math> element that starts SVG or MathML content).
// Recomputed from the ancestors with the parser's rules (HTML Standard
// section 13.2.6): <svg> and <math> open SVG and MathML content, and inside
// an integration point (SVG foreignObject, desc and title; MathML mi, mo, mn,
// ms and mtext, except for mglyph and malignmark; MathML annotation-xml with
// an HTML encoding; <svg> directly inside annotation-xml) the HTML rules apply
// again. The start tags that end foreign content never have an SVG or MathML
// parent, as the parser closes the foreign elements first.
ElementNs ForeignNsOf(const net_instaweb::HtmlElement* element) {
  struct Context {
    ElementNs ns = ElementNs::kHtml;
    bool html_ip = false;  // an HTML integration point
    bool text_ip = false;  // a MathML text integration point
    bool annotation_xml = false;
  };
  std::vector<const net_instaweb::HtmlElement*> chain;
  for (const net_instaweb::HtmlElement* p = element; p != nullptr;
       p = p->parent()) {
    chain.push_back(p);
  }
  Context parent;  // the document
  bool foreign_rules = false;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const net_instaweb::HtmlElement& e = **it;
    const bool html_rules =
        parent.ns == ElementNs::kHtml || parent.html_ip ||
        (parent.text_ip && !NameIs(e, "mglyph") && !NameIs(e, "malignmark")) ||
        (parent.annotation_xml && NameIs(e, "svg"));
    Context context;
    if (!html_rules) {
      context.ns = parent.ns;
    } else if (NameIs(e, "svg")) {
      context.ns = ElementNs::kSvg;
    } else if (NameIs(e, "math")) {
      context.ns = ElementNs::kMath;
    }
    if (context.ns == ElementNs::kSvg) {
      context.html_ip =
          NameIs(e, "foreignObject") || NameIs(e, "desc") || NameIs(e, "title");
    } else if (context.ns == ElementNs::kMath) {
      context.text_ip = NameIs(e, "mi") || NameIs(e, "mo") || NameIs(e, "mn") ||
                        NameIs(e, "ms") || NameIs(e, "mtext");
      context.annotation_xml = NameIs(e, "annotation-xml");
      if (context.annotation_xml) {
        // Only the first encoding attribute counts.
        for (const auto& attr : e.attributes()) {
          if (net_instaweb::StringCaseEqual(attr.name_str(), "encoding")) {
            const char* value = attr.DecodedValueOrNull();
            context.html_ip =
                value != nullptr &&
                (net_instaweb::StringCaseEqual(value, "text/html") ||
                 net_instaweb::StringCaseEqual(value, "application/xhtml+xml"));
            break;
          }
        }
      }
    }
    foreign_rules = !html_rules;
    parent = context;
  }
  return foreign_rules ? parent.ns : ElementNs::kHtml;
}

StyleScope ScopeOf(const net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;
  StyleScope scope;
  bool svg_or_math_ancestor = false;
  for (const net_instaweb::HtmlElement* p = element->parent(); p != nullptr;
       p = p->parent()) {
    switch (p->keyword()) {
      case HtmlName::kNoscript:
        scope.noscript = true;
        continue;
      case HtmlName::kTemplate:
      case HtmlName::kNoembed:
      case HtmlName::kNoframes:
        scope.inert = true;
        continue;
      default:
        break;
    }
    // <svg> and <math> are not interned keywords.
    if (NameIs(*p, "svg") || NameIs(*p, "math")) {
      svg_or_math_ancestor = true;
    }
  }
  if (svg_or_math_ancestor) {
    const ElementNs ns = ForeignNsOf(element);
    scope.svg = ns == ElementNs::kSvg;
    scope.math = ns == ElementNs::kMath;
  }
  return scope;
}

// The `type` rule the browser applies before it treats an element as CSS
// (Chromium's StyleElement::IsCSS and LinkStyle::Process): a <style> is CSS
// with no type, an empty one, or exactly "text/css" in any case; a stylesheet
// <link> is fetched with no type, an empty one, or the MIME type text/css
// (parameters such as "; charset=utf-8" allowed). `type="text/tailwindcss"`,
// "text/plain", "text/x-less" and the like are ignored by the browser, so
// their bytes are not the page's CSS.
bool StyleTypeIsCss(const char* type) {
  return type == nullptr || type[0] == '\0' ||
         net_instaweb::StringCaseEqual(type, "text/css");
}

bool LinkTypeIsCss(const char* type) {
  if (type == nullptr) return true;
  std::string_view mime(type);
  mime = mime.substr(0, mime.find(';'));
  mime = absl::StripAsciiWhitespace(mime);
  return mime.empty() || net_instaweb::StringCaseEqual(mime, "text/css");
}

// A stylesheet reference reduced to what identifies the fetched sheet, so two
// spellings of the same sheet compare equal ("/css/a.css", "css/a.css" on a
// root page, "http://example.com/css/a.css"): resolved against the document's
// base directory with the same ResolvePath the combined-stylesheet assembly
// uses, the scheme and fragment dropped, the host lowercased (the base URL's
// host for a reference without one: ResolvePath leaves a root-relative
// reference hostless, and under a cross-host <base href> the browser fetches
// it from the base's host, not the page's), and the path and
// query through NormalizeCacheUrl. Only ever compared with another identity;
// never fetched.
std::string SheetIdentity(std::string_view base_dir, std::string_view base_host,
                          std::string_view href) {
  href = absl::StripAsciiWhitespace(href);
  std::string resolved = ResolvePath(base_dir, href);
  std::string_view rest = resolved;
  std::string host;
  size_t scheme_end = rest.find("://");
  const bool has_authority =
      scheme_end != std::string_view::npos || rest.starts_with("//");
  if (has_authority) {
    rest.remove_prefix(scheme_end != std::string_view::npos ? scheme_end + 3
                                                            : 2);
    size_t host_end = rest.find_first_of("/?#");
    if (host_end == std::string_view::npos) host_end = rest.size();
    host = std::string(rest.substr(0, host_end));
    rest.remove_prefix(host_end);
  } else {
    host = std::string(base_host);
  }
  net_instaweb::AsciiToLowerInPlace(host);
  size_t fragment = rest.find('#');
  if (fragment != std::string_view::npos) rest = rest.substr(0, fragment);
  std::string path = rest.empty() || rest.front() != '/'
                         ? absl::StrCat("/", rest)
                         : std::string(rest);
  return absl::StrCat(host, NormalizeCacheUrl(path, UrlNormalizationConfig{}));
}

// Internal filter that collects element information during parsing.
// This extends EmptyHtmlFilter to track elements as they are encountered.
class ElementCollectorFilter : public net_instaweb::EmptyHtmlFilter {
 public:
  explicit ElementCollectorFilter(HtmlScanResult* result) : result_(result) {}

  [[nodiscard]] const char* Name() const override { return "ElementCollector"; }

  void set_page_url(std::string_view url) {
    page_origin_ = ExtractPageOrigin(url);
    page_url_ = std::string(url);
  }

  void StartDocument() override {
    depth_ = 0;
    element_index_ = 0;
    in_style_ = false;
    style_source_ = -1;
    source_css_bytes_ = 0;
    script_capture_ = false;
    script_text_overflow_ = false;
    script_text_.clear();
    in_body_ = false;
    hero_candidate_found_ = false;
    body_img_count_ = 0;
    lazy_hero_container_ = nullptr;
    lazy_hero_twin_seen_ = false;
    lazy_hero_stand_in_src_.clear();
    deferred_small_set_ = false;
    open_hero_containers_.clear();
    ancestor_is_hero_.clear();
    ancestor_inert_.clear();
    noscript_render_.clear();
    noscript_links_.clear();
    seen_origins_.clear();
    origin_entries_.clear();
    pending_stylesheets_.clear();
    link_source_pending_.clear();
  }

  void StartElement(net_instaweb::HtmlElement* element) override {
    net_instaweb::HtmlName::Keyword keyword = element->keyword();

    // The worker caches its OWN optimized output and re-scans THAT on
    // revalidation passes (the cache slot flips raw-origin <-> reprocessed).
    // The reprocessed variant carries nodes the raw-origin HTML never had — the
    // injected critical <style data-pagespeed-critical>, the async <noscript
    // data-pagespeed-async-fallback> + its child <link>, the async-loader
    // <script data-pagespeed-async-loader>, and preconnect/preload <link/script
    // data-pagespeed-hint>. TemplateDetector::HashStructure hashes the whole
    // element tree and CriticalCssExtractor keys criticality off element_index,
    // so collecting these injected nodes makes the scan differ between the raw
    // and reprocessed passes: the template hash flaps (a redundant browser
    // analysis + a wasted profile slot) and the index cutoff shifts. Excluding
    // them keeps result_->elements a fixed point.
    //
    // Only markers the worker puts on its OWN injected nodes belong here.
    // data-pagespeed-async / data-pagespeed-media (on the deferred primary
    // <link>) and data-pagespeed-defer (on a real <script>) sit on ORIGINAL
    // customer elements that ARE present in the raw scan, so they are
    // deliberately excluded from this set — skipping them would flap the hash
    // in the other direction. data-pagespeed-inlined (css_cache_inliner's
    // <style>) is likewise omitted on purpose: that inlined copy lives only in
    // the transient offline browser-analysis input, never in the cached/served
    // bytes the worker re-scans, so it cannot reach this scan. Bookkeeping below
    // (depth_, ancestor_is_hero_, stylesheet/SRI/origin collection) still runs
    // for injected nodes so the EndElement balance and the stylesheet/origin
    // lists stay correct.
    const bool worker_injected =
        element->FindAttribute("data-pagespeed-critical") != nullptr ||
        element->FindAttribute("data-pagespeed-hint") != nullptr ||
        element->FindAttribute("data-pagespeed-async-fallback") != nullptr ||
        element->FindAttribute("data-pagespeed-async-loader") != nullptr;

    // The customer's own <link>, async-deferred in place: it ships as
    // rel="preload" as="style" with its media parked in data-pagespeed-media.
    // Every place below that would recognize the raw <link rel="stylesheet">
    // has to recognize this too, or the scan of a reprocessed variant differs
    // from the scan of the raw origin and the derived state (stylesheet list,
    // template hash, preconnect priorities) flaps between passes.
    const bool deferred_primary =
        element->FindAttribute("data-pagespeed-async") != nullptr;

    // Collect element information
    CollectedElement collected;
    collected.tag_name = std::string(element->name_str());
    collected.depth = depth_;
    collected.hidden = IsInvisibleElement(*element);
    collected.may_exceed_viewport = MayExceedPhoneViewport(*element);
    // Advance element_index_ only for collected (non-injected) elements so a
    // real element keeps the SAME index across raw and reprocessed passes;
    // otherwise CriticalCssExtractor's "first N elements" cutoff
    // (element_index < max_elements) would not be a fixed point.
    if (!worker_injected) {
      collected.element_index = element_index_++;
    }

    // Extract id attribute
    const char* id_value = element->AttributeValue(net_instaweb::HtmlName::kId);
    if (id_value != nullptr) {
      collected.id = id_value;
    }

    // Extract class attribute and split into individual classes
    const char* class_value =
        element->AttributeValue(net_instaweb::HtmlName::kClass);
    if (class_value != nullptr) {
      std::vector<std::string_view> class_parts = absl::StrSplit(
          class_value, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty());
      for (const auto& part : class_parts) {
        collected.classes.push_back(std::string(part));
      }
    }

    // A browser running scripts never renders a <noscript> (its content is
    // raw text), and no browser renders <template> content or the raw text of
    // <noembed> and <noframes>. Such an element, and everything inside one, is
    // not part of the page the critical block is for, so it stays out of
    // `elements`: its classes would otherwise pull rules into
    // the block. It is still recorded for the template hash. The same names in
    // SVG and MathML content, where the parser gives them no keyword, stay out
    // too (streaming markup puts a <template> in an <svg>), as
    // StripNoscriptElements expects for its cross-check.
    const bool inert_name =
        keyword == net_instaweb::HtmlName::kNotAKeyword &&
        (NameIs(*element, "noscript") || NameIs(*element, "template") ||
         NameIs(*element, "noembed") || NameIs(*element, "noframes"));
    const bool inert = (!ancestor_inert_.empty() && ancestor_inert_.back()) ||
                       keyword == net_instaweb::HtmlName::kNoscript ||
                       keyword == net_instaweb::HtmlName::kTemplate ||
                       keyword == net_instaweb::HtmlName::kNoembed ||
                       keyword == net_instaweb::HtmlName::kNoframes ||
                       inert_name;
    ancestor_inert_.push_back(inert);
    TrackNoscriptRender(element, keyword, worker_injected);
    if (!worker_injected &&
        result_->elements.size() + result_->inert_elements.size() < 100000) {
      if (inert) {
        result_->inert_elements.push_back({result_->elements.size(),
                                           collected.depth,
                                           std::move(collected.tag_name)});
      } else {
        result_->elements.emplace_back(std::move(collected));
      }
    }

    // Check for stylesheet links
    const size_t pending_before = pending_stylesheets_.size();
    if (keyword == net_instaweb::HtmlName::kLink) {
      const char* rel = element->AttributeValue(net_instaweb::HtmlName::kRel);
      // Skip the worker's own async-CSS <noscript> fallback copy: it duplicates
      // the deferred primary <link> (same href). On a revalidation re-scan
      // of our own optimized output, collecting it as a second origin sheet
      // doubles the preload Early-Hints (worker.cc) and combined_css gather, and
      // flaps the num_stylesheets component of the template hash. Keeping only
      // the primary stabilizes the stylesheet LIST across raw-vs-reprocessed
      // passes; mirrors the data-pagespeed-critical <style> skip below. The
      // element-tree component of the template hash is stabilized separately, at
      // the top of StartElement, by excluding all worker-injected nodes from
      // result_->elements (the fallback link is one of them).
      //
      // A deferred primary is rel="preload" as="style", NOT rel="stylesheet" —
      // on a re-scan of our own output it is still the page's origin
      // stylesheet, so match it too or the reprocessed pass would see zero
      // stylesheets (the fallback copy being skipped just below): the template
      // hash would flap 1<->0 and the sheet would lose its Early-Hints entry.
      // Keyed on data-pagespeed-async so ordinary preloads (LCP image, fonts)
      // are not mistaken for stylesheets.
      const bool is_stylesheet =
          rel != nullptr &&
          (std::string_view(rel) == "stylesheet" ||
           (std::string_view(rel) == "preload" && deferred_primary));
      if (is_stylesheet &&
          element->FindAttribute("data-pagespeed-async-fallback") == nullptr) {
        StylesheetLink link;
        const char* href =
            element->AttributeValue(net_instaweb::HtmlName::kHref);
        if (href != nullptr) {
          link.href = href;
        }
        // For a deferred primary the author's media lives in
        // data-pagespeed-media; the live `media` attribute was removed (on a
        // preload it is a fetch condition, not the sheet's applicability).
        // Reporting the RECORDED media is what keeps media-dependent decisions
        // — chiefly StylesheetMediaIsPrint in the Early-Hints emitters — giving
        // the same answer on the raw and reprocessed passes. One value is
        // NORMALIZED rather than round-tripped: an author <link> with no media
        // scans as "" raw and as "all" reprocessed, because ApplyAsyncCss
        // records the default explicitly. Every media-keyed decision treats the
        // two identically (neither is "print"), so the decisions match; only
        // the string differs, and nothing downstream compares it for equality.
        const net_instaweb::HtmlElement::Attribute* media_attr =
            deferred_primary
                ? element->FindAttribute("data-pagespeed-media")
                : element->FindAttribute(net_instaweb::HtmlName::kMedia);
        const char* media = (media_attr != nullptr)
                                ? media_attr->DecodedValueOrNull()
                                : nullptr;
        if (media != nullptr) {
          link.media = media;
        }
        // Only a sheet that applies to a client running scripts is a source
        // of the page's CSS: a <noscript> sheet would otherwise
        // reach the combined stylesheet the inlined critical block is derived
        // from and be served to every browser. The one kept exception, the
        // loadCSS twin, is resolved in EndDocument once every preload is known.
        const StyleScope scope = ScopeOf(element);
        if (!LinkTypeIsCss(
                element->AttributeValue(net_instaweb::HtmlName::kType))) {
          // Not fetched as CSS by the browser, wherever it stands.
        } else if (scope.LinkApplies()) {
          link.print_media_swapped_on_load =
              !deferred_primary &&
              net_instaweb::StringCaseEqual(link.media, "print") &&
              HandlerSetsMedia(
                  element->AttributeValue(net_instaweb::HtmlName::kOnload));
          link.may_not_block_paint =
              !deferred_primary &&
              (in_body_ || link.print_media_swapped_on_load);
          pending_stylesheets_.emplace_back(std::move(link),
                                            PendingKind::kApplies);
        } else if (scope.NoscriptOnly()) {
          noscript_links_.push_back(pending_stylesheets_.size());
          pending_stylesheets_.emplace_back(std::move(link),
                                            PendingKind::kNoscriptTwin);
        }
      } else if (rel != nullptr && !deferred_primary &&
                 net_instaweb::StringCaseEqual(rel, "preload") &&
                 AttrValueEquals(
                     element->AttributeValue(net_instaweb::HtmlName::kAs),
                     "style") &&
                 element->FindAttribute("onload") != nullptr &&
                 element->FindAttribute("data-pagespeed-hint") == nullptr) {
        // An author's `rel=preload as=style` with an onload handler (loadCSS:
        // the handler flips this element's rel to stylesheet, so the sheet
        // applies HERE, in this element's position). A preload without one
        // is a fetch hint that no script applies; it never makes a
        // <noscript> twin count.
        const char* href =
            element->AttributeValue(net_instaweb::HtmlName::kHref);
        if (href != nullptr && href[0] != '\0' &&
            ScopeOf(element).LinkApplies()) {
          StylesheetLink link;
          link.href = href;
          const char* media =
              element->AttributeValue(net_instaweb::HtmlName::kMedia);
          if (media != nullptr) link.media = media;
          link.may_not_block_paint = true;
          pending_stylesheets_.emplace_back(std::move(link),
                                            PendingKind::kScriptLoaded);
        }
      }
    }

    // A <link> as a cascade-layer source, in document order. When the block
    // above queued it as a stylesheet (kApplies or kScriptLoaded), the source
    // carries that pending entry, which EndDocument maps to its index in
    // result_->stylesheets (or to none, when the loadCSS rule drops it).
    if (keyword == net_instaweb::HtmlName::kLink) {
      int pending_index = -1;
      if (pending_stylesheets_.size() > pending_before &&
          pending_stylesheets_.back().kind != PendingKind::kNoscriptTwin) {
        pending_index = static_cast<int>(pending_before);
      }
      RecordLinkSource(element, deferred_primary, pending_index);
    }
    if (keyword == net_instaweb::HtmlName::kScript && !worker_injected) {
      StartScript(element);
    }

    // SRI: collect URLs referenced with an integrity attribute (browsers
    // enforce the hash on <script> and <link> loads, so an optimized
    // variant served at the same URL would be refused).  Presence-based:
    // pinning on a malformed/empty integrity value only skips an
    // optimization, never breaks a page.
    if ((keyword == net_instaweb::HtmlName::kLink ||
         keyword == net_instaweb::HtmlName::kScript) &&
        element->FindAttribute(net_instaweb::HtmlName::kIntegrity) != nullptr &&
        result_->integrity_pinned_urls.size() < 1000) {
      const char* pinned_url =
          element->AttributeValue(keyword == net_instaweb::HtmlName::kLink
                                      ? net_instaweb::HtmlName::kHref
                                      : net_instaweb::HtmlName::kSrc);
      if (pinned_url != nullptr && pinned_url[0] != '\0') {
        result_->integrity_pinned_urls.emplace_back(pinned_url);
      }
    }

    // <script src> collection for the analysis resource map.  Like the
    // stylesheet/SRI/origin lists this runs for worker-injected nodes too
    // (an extra entry only adds a map candidate, never flaps the hash).
    if (keyword == net_instaweb::HtmlName::kScript &&
        result_->script_srcs.size() < 1000) {
      const char* script_src =
          element->AttributeValue(net_instaweb::HtmlName::kSrc);
      if (script_src != nullptr && script_src[0] != '\0') {
        result_->script_srcs.emplace_back(script_src);
      }
    }

    // Author <base href> detection.  An href-less <base target> sets no base
    // URL, so it must not suppress the analysis-time base injection.  Only the
    // first base-with-href sets the document base URL (HTML spec), so the
    // captured value is first-wins.
    if (keyword == net_instaweb::HtmlName::kBase && !result_->has_base_href) {
      const char* base_href =
          element->AttributeValue(net_instaweb::HtmlName::kHref);
      if (base_href != nullptr) {
        result_->has_base_href = true;
        result_->base_href = base_href;
      }
    }

    // Track when we enter a <style> tag (skip previously-injected
    // critical CSS to avoid duplication on revalidation passes).
    // Likewise skip a <style> that does not apply to a client running scripts
    // (inside <noscript>, <template> and the like), and one whose
    // type the browser does not treat as CSS.
    if (keyword == net_instaweb::HtmlName::kStyle) {
      if (element->FindAttribute("data-pagespeed-critical") == nullptr &&
          StyleTypeIsCss(
              element->AttributeValue(net_instaweb::HtmlName::kType)) &&
          ScopeOf(element).StyleApplies()) {
        in_style_ = true;
        // A <style> with a media attribute applies under that media only, so
        // its body joins `inline_css` inside an @media block of the same
        // media (on EndElement, once the whole body is read), never as
        // unconditional rules: a print <style> must not style the screen.
        const char* media =
            element->AttributeValue(net_instaweb::HtmlName::kMedia);
        const std::optional<std::string_view> scope =
            css::CombinedSheetMediaScope(media != nullptr ? media : "");
        style_media_unusable_ = !scope.has_value();
        style_media_scope_ = scope.has_value() ? std::string(*scope) : "";
        style_body_.clear();
        // The same <style> is a cascade-layer source, at this position.
        RecordStyleSource(element);
      }
    }

    ++depth_;

    // Track when we enter <body>
    if (keyword == net_instaweb::HtmlName::kBody) {
      in_body_ = true;
    }

    // Hero container tracking for LCP candidate detection
    bool is_hero_container = IsHeroContainer(keyword, class_value);

    bool ancestor_is_hero =
        !ancestor_is_hero_.empty() && ancestor_is_hero_.back();
    ancestor_is_hero_.push_back(is_hero_container || ancestor_is_hero);
    // depth_ was just advanced for this element's children; its own depth is
    // one less (what EndElement sees after its decrement).
    if (is_hero_container) {
      open_hero_containers_.push_back(
          {depth_ - 1, result_->lcp_candidate.script_loaded_hero});
    }

    // Origin extraction for preconnect hints.
    // Collect cross-origin origins with priority:
    //   - Render-blocking: <link rel="stylesheet" href>, <script src> (no async/defer)
    //   - Head resources: other <link href> elements
    //   - Body resources: <img src>, <script src> with async/defer
    {
      std::string_view src_url;
      int priority = kPriorityBodyResource;
      // Whether the motivating resource fetches in CORS mode.  Browsers key
      // connection reuse on the request mode, so the emitted preconnect must
      // carry crossorigin exactly when the resource does — otherwise it warms
      // a pool the page never uses.  Evidence: an explicit crossorigin
      // attribute, ES module scripts (always CORS), and font preloads (font
      // fetches are always CORS).  Plain stylesheets/scripts/images are
      // no-cors, which is also the fail-safe default.
      bool crossorigin = false;

      if (keyword == net_instaweb::HtmlName::kLink) {
        const char* href =
            element->AttributeValue(net_instaweb::HtmlName::kHref);
        if (href != nullptr && href[0] != '\0') {
          src_url = href;
          const char* rel =
              element->AttributeValue(net_instaweb::HtmlName::kRel);
          // A deferred primary is the page's stylesheet wearing a preload's
          // rel; classify it the same as the raw <link rel="stylesheet"> it
          // was, so a cross-origin sheet keeps its preconnect priority across
          // the raw and reprocessed passes.
          // A stylesheet inside <noscript> and the like never loads for a
          // client running scripts, so it blocks nothing.
          if (rel != nullptr &&
              (std::string_view(rel) == "stylesheet" ||
               (std::string_view(rel) == "preload" && deferred_primary)) &&
              ScopeOf(element).LinkApplies()) {
            priority = kPriorityRenderBlocking;
          } else {
            priority = in_body_ ? kPriorityBodyResource : kPriorityHeadResource;
          }
          crossorigin =
              element->FindAttribute(net_instaweb::HtmlName::kCrossorigin) !=
                  nullptr ||
              AttrValueEquals(
                  element->AttributeValue(net_instaweb::HtmlName::kAs), "font");
        }
      } else if (keyword == net_instaweb::HtmlName::kScript) {
        const char* src = element->AttributeValue(net_instaweb::HtmlName::kSrc);
        if (src != nullptr && src[0] != '\0') {
          src_url = src;
          bool is_async = element->FindAttribute("async") != nullptr;
          bool is_defer = element->FindAttribute("defer") != nullptr;
          if (!is_async && !is_defer && !in_body_) {
            priority = kPriorityRenderBlocking;
          } else {
            priority = in_body_ ? kPriorityBodyResource : kPriorityHeadResource;
          }
          crossorigin =
              element->FindAttribute(net_instaweb::HtmlName::kCrossorigin) !=
                  nullptr ||
              AttrValueEquals(
                  element->AttributeValue(net_instaweb::HtmlName::kType),
                  "module");
        }
      } else if (keyword == net_instaweb::HtmlName::kImg) {
        const char* src = element->AttributeValue(net_instaweb::HtmlName::kSrc);
        if (src != nullptr && src[0] != '\0') {
          src_url = src;
          priority = kPriorityBodyResource;
          crossorigin = element->FindAttribute(
                            net_instaweb::HtmlName::kCrossorigin) != nullptr;
        }
      }

      if (!src_url.empty()) {
        // First-seen wins: dedup decides BOTH inclusion and the crossorigin
        // bit.  For a mixed-mode origin (fetched both no-cors and CORS),
        // warming one of its pools still beats the old always-crossorigin
        // behavior, and one entry per origin keeps the 4-entry budget simple.
        std::string origin = ExtractOrigin(src_url);
        if (!origin.empty() && origin != page_origin_ &&
            seen_origins_.find(origin) == seen_origins_.end()) {
          seen_origins_.insert(origin);
          origin_entries_.push_back({std::move(origin), priority, crossorigin});
        }
      }
    }

    // LCP candidate detection for <img> in body.  Beacon/hidden images
    // (1x1 tracking pixels etc.) are never the LCP element — considering
    // them would preload a beacon at high priority ahead of real content.
    // Neither is an <img> inside <noscript>, <template>, <noembed> or
    // <noframes>: a browser running scripts never creates it
    // (the lazy-load fallback `<img data-src=hero.jpg class=lazy>
    // <noscript><img src=hero.jpg></noscript>`), so a preload and Early Hint
    // for it fetch an image the page never shows. It does not count toward
    // the fallback window either; element_index_ has already advanced for
    // it, so lcp_candidate.element_index stays comparable with `elements`.
    //
    // A script-loaded hero is the one shape that yields no
    // candidate at all: the first hero container holds a lazy-load
    // placeholder, an <img> without a usable src that a loader fills in
    // (`<img data-src=hero.jpg class=lazy>`), followed by its <noscript>
    // copy, and no eligible image of its own. The hero is what the browser
    // will paint largest, and nothing in the markup says which bytes the
    // loader fetches (data-src is a convention; the <noscript> twin's URL may
    // differ in size or format), so there is nothing right to hint. Falling
    // through to the next image, as the fallback below would, hints an image
    // that is probably below the fold at the highest priority, which is
    // worse than no hint. The decision drops an earlier fallback (a real
    // hero image would have overridden it) and rules out every later image
    // except one in a hero container opened after the decision: a lazy
    // logo with its copy alone in <header> must not silence the real hero
    // in the <section> after it, while an image elsewhere inside the <main>
    // that wraps the lazy hero stays unhinted.
    //
    // Two refinements. A placeholder may carry a real-looking
    // src: a stand-in named like one (`/blank.gif`, IsLazyLoadPlaceholder's
    // second leg) is a placeholder outright; any other src with a lazy data
    // attribute is taken as the hero candidate, as before, but provisionally
    // (lazy_hero_stand_in_src_): should its <noscript> copy carry a different
    // src, the copy says what the real image is, the candidate is withdrawn
    // and the <img> is the pending placeholder from then on. And a small
    // eligible image inside the placeholder's container (a 48x48 icon in the
    // call-to-action, IsSmallDeclaredImage) is set aside rather than taken:
    // it cancels nothing, and it is the candidate only if the pattern is not
    // confirmed by the copy, which is what the first-eligible-image rule
    // picked before, so a page without the copy sees no change.
    if (in_body_ && keyword == net_instaweb::HtmlName::kImg) {
      if (inert) {
        // The <noscript> copy of a pending placeholder. While the placeholder's
        // hero container is open, every element seen is inside it.
        // The copy has to be a plausible one: a <noscript> image the markup
        // declares invisible (a 1x1 tracking pixel's no-JS fallback, a
        // hidden image) in the same landmark is not the author's copy of
        // the hero and confirms nothing.
        if (lazy_hero_container_ != nullptr && !lazy_hero_twin_seen_ &&
            ScopeOf(element).noscript && !IsUnlikelyLcpImage(*element)) {
          if (lazy_hero_stand_in_src_.empty()) {
            lazy_hero_twin_seen_ = true;
          } else if (const char* twin_src =
                         element->AttributeValue(net_instaweb::HtmlName::kSrc);
                     twin_src != nullptr && twin_src[0] != '\0' &&
                     ImageIdentity(lazy_hero_stand_in_src_) !=
                         ImageIdentity(twin_src)) {
            // "Different" is judged on the resolved reference (host, path
            // and query; scheme and fragment dropped), so `/hero.jpg` and
            // `http://example.com/hero.jpg` are the same image, while
            // `/hero.jpg?v=2` stays a different one.
            // The provisional candidate was a stand-in after all. Withdraw
            // it; the container's close decides, unless an eligible image
            // of the container's own comes first and takes its place.
            lazy_hero_twin_seen_ = true;
            lazy_hero_stand_in_src_.clear();
            result_->lcp_candidate = LcpCandidate{};
            hero_candidate_found_ = false;
          }
        }
      } else if (IsLazyLoadPlaceholder(element)) {
        // The container has to be an ancestor: the <img>'s own hero class
        // (which ancestor_is_hero_.back() includes) makes no container that
        // could hold the <noscript> copy, so such a placeholder starts
        // nothing and the heuristic goes on as before.
        if (ancestor_is_hero && !hero_candidate_found_ &&
            !result_->lcp_candidate.script_loaded_hero &&
            lazy_hero_container_ == nullptr && !IsUnlikelyLcpImage(*element)) {
          lazy_hero_container_ = InnermostHeroContainer(element);
          lazy_hero_twin_seen_ = false;
          lazy_hero_stand_in_src_.clear();
          deferred_small_set_ = false;
        }
      } else if (const char* src =
                     element->AttributeValue(net_instaweb::HtmlName::kSrc);
                 src != nullptr &&
                 !std::string_view(src).starts_with("data:")) {
        const char* srcset =
            element->AttributeValue(net_instaweb::HtmlName::kSrcset);
        const auto* sizes_attr = element->FindAttribute("sizes");
        const char* sizes_val = (sizes_attr != nullptr)
                                    ? sizes_attr->DecodedValueOrNull()
                                    : nullptr;
        auto candidate_for = [&](LcpCandidate* candidate) {
          candidate->script_loaded_hero = false;
          candidate->src = src;
          candidate->srcset = (srcset != nullptr) ? srcset : "";
          candidate->sizes = (sizes_val != nullptr) ? sizes_val : "";
          candidate->element_index = element_index_ - 1;
          candidate->in_picture = ParentIsPicture(element);
        };

        bool in_hero = !ancestor_is_hero_.empty() && ancestor_is_hero_.back();
        bool eligible = !IsUnlikelyLcpImage(*element);
        // After a script-loaded hero decision, only an image whose innermost
        // hero container was opened since (a later sibling <section>, say,
        // at any depth) can still be the candidate. A container already open
        // at the decision (the <main> around everything) is the hero's own
        // surroundings, and an image outside any hero container is the
        // fallback the decision rules out.
        const bool decided = result_->lcp_candidate.script_loaded_hero &&
                             !(in_hero && !open_hero_containers_.empty() &&
                               open_hero_containers_.back().fresh);
        // While a placeholder is pending, everything seen is inside its hero
        // container.
        const bool pending = lazy_hero_container_ != nullptr;
        const bool small =
            pending && eligible && IsSmallDeclaredImage(*element);
        if (pending && eligible && !small) {
          // An eligible image of the container's own: the container holds
          // more than the placeholder, so it is not a script-loaded hero.
          // A small image set aside before it, with the copy not seen, is
          // what the first-eligible-image rule picked before and still does.
          lazy_hero_container_ = nullptr;
          lazy_hero_stand_in_src_.clear();
          if (deferred_small_set_ && !lazy_hero_twin_seen_ &&
              !hero_candidate_found_) {
            result_->lcp_candidate = deferred_small_;
            hero_candidate_found_ = true;
          }
          deferred_small_set_ = false;
        }
        if (decided) {
          // No candidate for this page.
        } else if (small) {
          // Set aside, not taken: resolved when the pending placeholder is.
          if (!deferred_small_set_) {
            candidate_for(&deferred_small_);
            deferred_small_set_ = true;
          }
        } else if (eligible && in_hero && !hero_candidate_found_) {
          // Hero candidate: override any previous fallback, and a
          // script-loaded hero decision too.
          candidate_for(&result_->lcp_candidate);
          hero_candidate_found_ = true;
          // With a lazy data attribute this may be a stand-in whose real
          // image the <noscript> copy names; its container is watched as
          // for a placeholder, the candidate standing until a copy with a
          // different src withdraws it.
          if (ancestor_is_hero && HasLazySourceAttribute(*element)) {
            lazy_hero_container_ = InnermostHeroContainer(element);
            lazy_hero_twin_seen_ = false;
            lazy_hero_stand_in_src_ = src;
            deferred_small_set_ = false;
          }
        } else if (eligible && !hero_candidate_found_ && body_img_count_ < 50 &&
                   result_->lcp_candidate.src.empty()) {
          // Fallback: first body image (if no hero found yet)
          candidate_for(&result_->lcp_candidate);
        }
        ++body_img_count_;
      }
    }
  }

  void EndElement(net_instaweb::HtmlElement* element) override {
    if (depth_ > 0) --depth_;

    // Pop the hero ancestor stack
    if (!ancestor_is_hero_.empty()) {
      ancestor_is_hero_.pop_back();
    }
    // The hero container of a pending lazy-load placeholder closes.
    // No eligible image of its own cancelled the placeholder on the
    // way; with the <noscript> copy seen too, this is a script-loaded hero
    // and the page gets no candidate. Without the copy the placeholder is
    // not the pattern, and the heuristic goes on as before.
    // A small image set aside inside the container is dropped
    // with the copy seen, and is otherwise the candidate the first-eligible
    // rule would have picked, unless a provisional stand-in already is.
    if (element == lazy_hero_container_) {
      lazy_hero_container_ = nullptr;
      lazy_hero_stand_in_src_.clear();
      if (lazy_hero_twin_seen_) {
        result_->lcp_candidate = LcpCandidate{};
        result_->lcp_candidate.script_loaded_hero = true;
        hero_candidate_found_ = false;
      } else if (deferred_small_set_ && !hero_candidate_found_) {
        result_->lcp_candidate = deferred_small_;
        hero_candidate_found_ = true;
      }
      deferred_small_set_ = false;
    }
    // depth_ is the closing element's own depth again.
    if (!open_hero_containers_.empty() &&
        open_hero_containers_.back().depth == depth_) {
      open_hero_containers_.pop_back();
    }
    if (!ancestor_inert_.empty()) ancestor_inert_.pop_back();
    if (!noscript_render_.empty()) noscript_render_.pop_back();

    // Track when we exit a <style> tag
    if (element->keyword() == net_instaweb::HtmlName::kStyle) {
      if (in_style_ && !style_media_scope_.empty() && !style_body_.empty() &&
          result_->inline_css.size() + 1 +
                  css::MediaScopedCssSize(style_media_scope_,
                                          style_body_.size()) <=
              kMaxInlineCssBytes) {
        // A body that cannot be wrapped in its media safely adds nothing.
        const size_t before = result_->inline_css.size();
        if (!result_->inline_css.empty()) result_->inline_css += "\n";
        if (!css::AppendMediaScopedCss(style_media_scope_, style_body_,
                                       &result_->inline_css)) {
          result_->inline_css.resize(before);
        }
      }
      style_body_.clear();
      style_media_scope_.clear();
      style_media_unusable_ = false;
      in_style_ = false;
      style_source_ = -1;
    }
    if (element->keyword() == net_instaweb::HtmlName::kScript) {
      FinishScript();
    }

    // Track when we exit <body>
    if (element->keyword() == net_instaweb::HtmlName::kBody) {
      in_body_ = false;
    }
  }

  void EndDocument() override {
    // Stylesheets in document order. A script-loaded preload (loadCSS) counts
    // as a sheet, at the preload's own position, only when a <noscript> link
    // declares the same sheet, no applying link already does, and only once.
    // Sheets are matched by SheetIdentity, not by the raw href: resolved
    // against the document's base (BaseOf: the <base href> when it has one),
    // and a hostless href takes the base URL's host, as the browser does
    // (under a cross-host <base href> "/css/a.css" is the base host's sheet).
    // The twin contributes its media when the preload has none.
    // The worker's own twin (data-pagespeed-async-fallback) never gets here:
    // its primary is the deferred link, which is collected as the stylesheet
    // itself.
    const DocumentBase base = BaseOf();
    std::unordered_set<std::string> declared;
    std::unordered_map<std::string, std::string> twin_media;
    std::vector<std::string> identities;
    identities.reserve(pending_stylesheets_.size());
    for (const PendingStylesheet& p : pending_stylesheets_) {
      identities.push_back(SheetIdentity(base.dir, base.host, p.link.href));
      if (p.kind == PendingKind::kApplies) {
        declared.insert(identities.back());
      } else if (p.kind == PendingKind::kNoscriptTwin) {
        twin_media.emplace(identities.back(), p.link.media);
      }
    }
    // A <noscript> stylesheet link changes a no-JS render unless the sheet
    // applies for a client running scripts anyway: declared by a link that
    // applies, or the loadCSS twin of a script-loaded preload.
    if (!result_->noscript_affects_render) {
      std::unordered_set<std::string> preloaded;
      for (size_t i = 0; i < pending_stylesheets_.size(); ++i) {
        if (pending_stylesheets_[i].kind == PendingKind::kScriptLoaded) {
          preloaded.insert(identities[i]);
        }
      }
      for (size_t i : noscript_links_) {
        if (!declared.contains(identities[i]) &&
            !preloaded.contains(identities[i])) {
          result_->noscript_affects_render = true;
          break;
        }
      }
    }
    std::unordered_set<std::string> script_loaded;
    // Where each pending entry landed in result_->stylesheets, or -1.
    std::vector<int> stylesheet_index(pending_stylesheets_.size(), -1);
    for (size_t i = 0; i < pending_stylesheets_.size(); ++i) {
      PendingStylesheet& p = pending_stylesheets_[i];
      if (p.kind == PendingKind::kNoscriptTwin) continue;
      if (p.kind == PendingKind::kScriptLoaded) {
        auto twin = twin_media.find(identities[i]);
        if (twin == twin_media.end() || declared.contains(identities[i]) ||
            !script_loaded.insert(identities[i]).second) {
          continue;
        }
        if (p.link.media.empty()) p.link.media = twin->second;
      }
      stylesheet_index[i] = static_cast<int>(result_->stylesheets.size());
      result_->stylesheets.push_back(std::move(p.link));
    }
    // The cascade-layer sources of those links point at the same entries, so
    // the layer order reads exactly the sheets the combined CSS reads. A
    // loadCSS preload the rule above drops keeps -1: a browser still applies
    // it, but its sheet is not gathered, so the order is not proven.
    for (const auto& [source, pending] : link_source_pending_) {
      result_->stylesheet_sources[source].stylesheet_index =
          stylesheet_index[pending];
    }

    // Sort origins by priority (render-blocking first) and cap at 4.
    std::stable_sort(origin_entries_.begin(), origin_entries_.end(),
                     [](const OriginEntry& a, const OriginEntry& b) {
                       return a.priority < b.priority;
                     });
    size_t count = std::min(origin_entries_.size(), size_t{4});
    for (size_t i = 0; i < count; ++i) {
      result_->third_party_origins.push_back(
          {std::move(origin_entries_[i].origin),
           origin_entries_[i].crossorigin});
    }
  }

  void Characters(net_instaweb::HtmlCharactersNode* characters) override {
    // Text a no-JS render of a <noscript> shows.
    if (!result_->noscript_affects_render && !noscript_render_.empty() &&
        noscript_render_.back() == NoscriptRender::kVisible &&
        !absl::StripAsciiWhitespace(characters->contents()).empty()) {
      result_->noscript_affects_render = true;
    }
    if (script_capture_) {
      std::string_view text = characters->contents();
      if (script_text_.size() + text.size() <= kMaxScriptTextBytes) {
        script_text_.append(text);
      } else {
        script_text_overflow_ = true;
      }
    }
    // Collect content of <style> tags
    if (in_style_) {
      std::string_view content = characters->contents();
      if (!style_media_scope_.empty()) {
        // Held until EndElement wraps the whole body in its @media block.
        if (style_body_.size() + content.size() <= kMaxInlineCssBytes) {
          if (!style_body_.empty()) style_body_ += "\n";
          style_body_ += content;
        }
      } else if (style_media_unusable_) {
        // A media value no browser parses: the <style> applies nowhere.
      } else if (result_->inline_css.size() + content.size() <=
                 kMaxInlineCssBytes) {
        // Cap inline CSS at 2MB to prevent unbounded growth on malformed
        // HTML.
        if (!result_->inline_css.empty()) {
          result_->inline_css += "\n";
        }
        result_->inline_css += content;
      }
      if (style_source_ >= 0) {
        StylesheetSource& source = result_->stylesheet_sources[style_source_];
        if (source_css_bytes_ + content.size() <= kMaxSourceCssBytes) {
          source.css += content;
          source_css_bytes_ += content.size();
        } else {
          source.truncated = true;
        }
      }
    }
  }

 private:
  // The cap on the combined inline CSS.
  static constexpr size_t kMaxInlineCssBytes = size_t{2} * 1024 * 1024;
  // The same cap as the combined inline CSS.
  static constexpr size_t kMaxSourceCssBytes = kMaxInlineCssBytes;

  // A <style> that StartElement's stylesheet-source rule collects into the
  // combined inline
  // CSS, recorded as a cascade-layer source at its position. Characters fills
  // its body.
  void RecordStyleSource(const net_instaweb::HtmlElement* element) {
    StylesheetSource source;
    // A titled sheet belongs to a sheet set; a browser disables every set but
    // the preferred one.
    source.titled = element->FindAttribute("title") != nullptr;
    const char* media = element->AttributeValue(net_instaweb::HtmlName::kMedia);
    if (media != nullptr) source.media = media;
    style_source_ = static_cast<int>(result_->stylesheet_sources.size());
    result_->stylesheet_sources.push_back(std::move(source));
  }

  // A <link> as a cascade-layer source. Which links are stylesheets, and at
  // which position, is the stylesheet-source rule above (StyleScope,
  // LinkTypeIsCss, the
  // loadCSS preload at its own position): `pending_index` is the
  // pending_stylesheets_ entry StartElement queued for this link, or -1.
  // EndDocument maps it to the link's index in result_->stylesheets, or to
  // none when the loadCSS rule drops it (no <noscript> twin, or the sheet is
  // already declared).
  //
  // A link that rule does not queue is still a source when a browser running
  // scripts applies it: a rel token list that names `stylesheet` in a
  // spelling the gather does not match (`Stylesheet`, `alternate
  // stylesheet`), or a `preload` among other tokens with as=style and an
  // onload handler. Such a source has no gathered sheet, so it leaves the
  // order unproven rather than unseen.
  void RecordLinkSource(const net_instaweb::HtmlElement* element,
                        bool deferred_primary, int pending_index) {
    using net_instaweb::HtmlName;
    const char* rel = element->AttributeValue(HtmlName::kRel);
    const std::string_view rel_view = rel != nullptr ? rel : "";
    if (pending_index < 0) {
      // The worker's own <noscript> copy and preload hints are not sheets.
      if (element->FindAttribute("data-pagespeed-async-fallback") != nullptr ||
          element->FindAttribute("data-pagespeed-hint") != nullptr ||
          deferred_primary ||
          !LinkTypeIsCss(element->AttributeValue(HtmlName::kType)) ||
          !ScopeOf(element).LinkApplies()) {
        return;
      }
      const bool applies =
          RelHasToken(rel_view, "stylesheet") ||
          (RelHasToken(rel_view, "preload") &&
           AttrValueEquals(element->AttributeValue(HtmlName::kAs), "style") &&
           element->FindAttribute("onload") != nullptr);
      if (!applies) return;
    }

    StylesheetSource source;
    source.is_link = true;
    source.titled = element->FindAttribute("title") != nullptr;
    const char* href = element->AttributeValue(HtmlName::kHref);
    if (href != nullptr) source.href = href;
    const net_instaweb::HtmlElement::Attribute* media_attr =
        deferred_primary ? element->FindAttribute("data-pagespeed-media")
                         : element->FindAttribute(HtmlName::kMedia);
    const char* media =
        media_attr != nullptr ? media_attr->DecodedValueOrNull() : nullptr;
    if (media != nullptr) source.media = media;
    source.not_plain_stylesheet = RelHasToken(rel_view, "alternate") ||
                                  element->FindAttribute("disabled") != nullptr;
    if (pending_index >= 0) {
      link_source_pending_.emplace_back(result_->stylesheet_sources.size(),
                                        static_cast<size_t>(pending_index));
    }
    result_->stylesheet_sources.push_back(std::move(source));
  }

  // A <script> that may insert a stylesheet where it stands, recorded among
  // the stylesheet sources (StylesheetSource::may_insert_stylesheet): a
  // parser-blocking external classic script (it can document.write one), or
  // an inline script whose text shows it builds one. Async, deferred and module
  // scripts from a URL run after parsing and are not recorded (residual risk:
  // they may still insert a sheet by DOM calls). A script the worker deferred
  // (`defer data-pagespeed-defer`, HtmlTransformFilter::ApplyScriptDeferral,
  // which only defers a parser-blocking script) is read as the parser-blocking
  // script it was, like a deferred stylesheet (data-pagespeed-async), so the
  // scan of the worker's own output gives the order of the original page.
  // Called at the script's start tag; an inline script is decided at its end
  // tag (FinishScript).
  void StartScript(net_instaweb::HtmlElement* element) {
    using net_instaweb::HtmlName;
    script_capture_ = false;
    if (!ScopeOf(element).LinkApplies()) return;
    const char* type = element->AttributeValue(HtmlName::kType);
    const bool module =
        type != nullptr && net_instaweb::StringCaseEqual(
                               absl::StripAsciiWhitespace(type), "module");
    if (type != nullptr && !module && !IsJavaScriptMimeType(type)) return;
    if (element->FindAttribute(HtmlName::kSrc) != nullptr) {
      const bool worker_deferred =
          element->FindAttribute("data-pagespeed-defer") != nullptr;
      if (!module && element->FindAttribute(HtmlName::kAsync) == nullptr &&
          (worker_deferred ||
           element->FindAttribute(HtmlName::kDefer) == nullptr)) {
        PushScriptSource();
      }
      return;
    }
    script_capture_ = true;
    script_text_.clear();
  }

  void FinishScript() {
    if (!script_capture_) return;
    script_capture_ = false;
    static constexpr std::string_view kSigns[] = {
        "document.write", "insertRule", "@layer", "adoptedStyleSheets",
        "CSSStyleSheet"};
    bool may_insert = script_text_overflow_;
    for (std::string_view sign : kSigns) {
      if (script_text_.find(sign) != std::string::npos) may_insert = true;
    }
    // createElement('style') / createElement("link") and the like.
    for (size_t at = script_text_.find("createElement(");
         !may_insert && at != std::string::npos;
         at = script_text_.find("createElement(", at + 1)) {
      std::string_view arg = std::string_view(script_text_).substr(at + 14, 12);
      std::string lower(arg);
      net_instaweb::AsciiToLowerInPlace(lower);
      if (lower.find("style") != std::string::npos ||
          lower.find("link") != std::string::npos) {
        may_insert = true;
      }
    }
    script_text_.clear();
    script_text_overflow_ = false;
    if (may_insert) PushScriptSource();
  }

  void PushScriptSource() {
    StylesheetSource source;
    source.may_insert_stylesheet = true;
    result_->stylesheet_sources.push_back(std::move(source));
  }

  // True for a script type a browser runs as classic JavaScript: empty, or one
  // of the JavaScript MIME types (parameters ignored).
  static bool IsJavaScriptMimeType(std::string_view type) {
    std::string_view t = absl::StripAsciiWhitespace(type);
    t = absl::StripAsciiWhitespace(t.substr(0, t.find(';')));
    if (t.empty()) return true;
    for (std::string_view js :
         {"text/javascript", "application/javascript", "text/ecmascript",
          "application/ecmascript", "application/x-javascript",
          "application/x-ecmascript", "text/x-javascript", "text/x-ecmascript",
          "text/jscript", "text/livescript", "text/javascript1.0",
          "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
          "text/javascript1.4", "text/javascript1.5"}) {
      if (net_instaweb::StringCaseEqual(t, js)) return true;
    }
    return false;
  }

  // True when the space-separated token list `rel` has `token`
  // (case-insensitive).
  static bool RelHasToken(std::string_view rel, std::string_view token) {
    std::vector<std::string_view> tokens =
        absl::StrSplit(rel, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty());
    for (std::string_view t : tokens) {
      if (net_instaweb::StringCaseEqual(t, token)) return true;
    }
    return false;
  }

  // Returns true if any class token matches a hero-related pattern.
  // A token matches if it equals the pattern or starts with "pattern-"
  // (hyphen-delimited prefix). Case-insensitive.
  static bool HasHeroClass(const char* class_value) {
    if (class_value == nullptr) return false;
    // Split into individual class tokens
    std::vector<std::string_view> tokens = absl::StrSplit(
        class_value, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty());

    static constexpr std::string_view kPatterns[] = {"hero", "banner",
                                                     "masthead", "above-fold"};

    for (const auto& token : tokens) {
      std::string lower_token(token);
      net_instaweb::AsciiToLowerInPlace(lower_token);
      for (const auto& pattern : kPatterns) {
        // Exact match or prefix match with hyphen separator
        if (lower_token == pattern || (lower_token.starts_with(pattern) &&
                                       lower_token.size() > pattern.size() &&
                                       lower_token[pattern.size()] == '-')) {
          return true;
        }
      }
    }
    return false;
  }

  // Origin entry with priority for sorting.
  struct OriginEntry {
    std::string origin;
    int priority;
    bool crossorigin;
  };

  HtmlScanResult* result_;
  int depth_{0};
  int element_index_{0};
  bool in_style_{false};
  // For the open <style>: the media its body is scoped to in `inline_css`
  // ("" when it applies as written), the body read so far when it is scoped,
  // and whether its media value is one no browser parses.
  std::string style_media_scope_;
  std::string style_body_;
  bool style_media_unusable_{false};
  // Index in stylesheet_sources of the <style> being read, or -1.
  int style_source_{-1};
  size_t source_css_bytes_{0};
  // The inline <script> being read by StartScript / FinishScript.
  static constexpr size_t kMaxScriptTextBytes = size_t{1} * 1024 * 1024;
  bool script_capture_{false};
  bool script_text_overflow_{false};
  std::string script_text_;

  // Per open element: it is inside, or is, a subtree no browser running
  // scripts renders (see the `inert` rule in StartElement).
  std::vector<bool> ancestor_inert_;

  // Does the page's <noscript> content change what a render
  // with script execution disabled shows? The analysis renders now leave it
  // out, so a validation record made while they rendered it is about another
  // fold; HtmlScanResult::noscript_affects_render salts the record's binding
  // for exactly those pages. Per open element: outside an author <noscript>,
  // inside one where it renders, or inside one but not rendered (an
  // invisible element and what it holds; script, style and the like).
  enum class NoscriptRender : uint8_t { kOutside, kVisible, kHidden };
  std::vector<NoscriptRender> noscript_render_;
  // pending_stylesheets_ indices of <noscript> stylesheet links.
  std::vector<size_t> noscript_links_;

  void TrackNoscriptRender(const net_instaweb::HtmlElement* element,
                           net_instaweb::HtmlName::Keyword keyword,
                           bool worker_injected) {
    using net_instaweb::HtmlName;
    const NoscriptRender parent = noscript_render_.empty()
                                      ? NoscriptRender::kOutside
                                      : noscript_render_.back();
    NoscriptRender state = parent;
    if (parent == NoscriptRender::kOutside) {
      // An author <noscript> that a scripting browser reads as raw text (not
      // the worker's own fallback copy, not in foreign content or inert
      // subtrees, where nothing changes).
      if (keyword == HtmlName::kNoscript && !worker_injected &&
          ScopeOf(element).LinkApplies()) {
        state = NoscriptRender::kVisible;
      }
      noscript_render_.push_back(state);
      return;
    }
    // Inside an author <noscript>. A <style> applies wherever it stands.
    if (keyword == HtmlName::kStyle && !worker_injected &&
        StyleTypeIsCss(element->AttributeValue(HtmlName::kType))) {
      result_->noscript_affects_render = true;
    }
    // A refresh navigates a JS-off render away (with every request blocked,
    // to the browser's error page), wherever it stands.
    if (keyword == HtmlName::kMeta && !worker_injected &&
        AttrValueEquals(element->AttributeValue(HtmlName::kHttpEquiv),
                        "refresh")) {
      result_->noscript_affects_render = true;
    }
    if (parent == NoscriptRender::kVisible) {
      const bool renders_nothing =
          keyword == HtmlName::kScript || keyword == HtmlName::kStyle ||
          keyword == HtmlName::kLink || keyword == HtmlName::kMeta ||
          keyword == HtmlName::kTitle || keyword == HtmlName::kBase ||
          keyword == HtmlName::kTemplate || keyword == HtmlName::kNoscript;
      if (renders_nothing || worker_injected || IsInvisibleElement(*element)) {
        // A tracking pixel or a hidden tag-manager iframe (the GTM snippet)
        // shows nothing, with or without scripts.
        state = NoscriptRender::kHidden;
      } else {
        result_->noscript_affects_render = true;
      }
    }
    noscript_render_.push_back(state);
  }

  // LCP candidate tracking
  std::vector<bool> ancestor_is_hero_;
  bool in_body_ = false;
  bool hero_candidate_found_ = false;
  int body_img_count_ = 0;
  // The innermost hero container of a lazy-load placeholder seen
  // before any hero candidate, while that container is open (compared by
  // identity only; never dereferenced after its EndElement), and whether its
  // <noscript> copy has been seen since.
  const net_instaweb::HtmlElement* lazy_hero_container_ = nullptr;
  bool lazy_hero_twin_seen_ = false;
  // Non-empty while the pending placeholder is a provisional
  // stand-in, an <img> with a real src and a lazy data attribute that is the
  // hero candidate until a <noscript> copy with a different src withdraws it.
  std::string lazy_hero_stand_in_src_;
  // The first small eligible image (IsSmallDeclaredImage) seen inside the
  // pending placeholder's container, set aside until the placeholder resolves:
  // dropped when the copy confirms the pattern, else the candidate the
  // first-eligible-image rule would have picked.
  LcpCandidate deferred_small_;
  bool deferred_small_set_ = false;
  // Every open hero container, innermost last: its own depth (to pop it) and
  // whether it was opened after a script-loaded hero decision. An image whose
  // innermost hero container is such a fresh one may still be the candidate;
  // one in a container already open at the decision may not. Judged by when
  // the container opened, not by depth: a fresh sibling can be shallower than
  // the decided container (`<main><div class=hero>..</div></main><section>`).
  struct OpenHeroContainer {
    int depth;
    bool fresh;
  };
  std::vector<OpenHeroContainer> open_hero_containers_;

  // The hero containers of the LCP heuristic: the landmark elements a page's
  // top content sits in, and anything whose class says so (HasHeroClass).
  static bool IsHeroContainer(net_instaweb::HtmlName::Keyword keyword,
                              const char* class_value) {
    return keyword == net_instaweb::HtmlName::kHeader ||
           keyword == net_instaweb::HtmlName::kMain ||
           keyword == net_instaweb::HtmlName::kSection ||
           keyword == net_instaweb::HtmlName::kArticle ||
           HasHeroClass(class_value);
  }

  // The nearest ancestor that IsHeroContainer, or null. Ancestors only: an
  // element's own hero class does not count here, so the caller checks that
  // an ancestor is a hero container (`ancestor_is_hero`, computed before the
  // element's own entry is pushed), and then there is one.
  static const net_instaweb::HtmlElement* InnermostHeroContainer(
      const net_instaweb::HtmlElement* element) {
    for (const net_instaweb::HtmlElement* p = element->parent(); p != nullptr;
         p = p->parent()) {
      if (IsHeroContainer(p->keyword(),
                          p->AttributeValue(net_instaweb::HtmlName::kClass))) {
        return p;
      }
    }
    return nullptr;
  }

  // A lazy-load placeholder: an <img> with no usable src (none,
  // empty, or a data: stand-in) that a loader fills in, which is said by a
  // `data-*` attribute carrying the real source (data-src, data-srcset,
  // data-lazy-src, data-original and the like, HasLazySourceAttribute) or by a
  // lazy-loader class (`lazy`, `lazyload`, `lazyloaded`, or a `lazy-` prefixed
  // token). Also an <img> with such a data attribute whose src
  // is a file named like a stand-in (`/blank.gif`, IsLazyStandInSrc); a class
  // alone does not make a real src a stand-in. The placeholder is the
  // author's, not a loading="lazy" the worker adds. The third leg on
  // LcpCandidate, a copy with a different src, is decided by the caller as
  // the copy arrives.
  static bool IsLazyLoadPlaceholder(const net_instaweb::HtmlElement* element) {
    const char* src = element->AttributeValue(net_instaweb::HtmlName::kSrc);
    if (src != nullptr && src[0] != '\0' &&
        !std::string_view(src).starts_with("data:")) {
      return HasLazySourceAttribute(*element) && IsLazyStandInSrc(src);
    }
    if (HasLazySourceAttribute(*element)) return true;
    const char* class_value =
        element->AttributeValue(net_instaweb::HtmlName::kClass);
    if (class_value == nullptr) return false;
    std::vector<std::string_view> tokens = absl::StrSplit(
        class_value, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty());
    for (std::string_view token : tokens) {
      std::string lower(token);
      net_instaweb::AsciiToLowerInPlace(lower);
      if (lower == "lazy" || lower == "lazyload" || lower == "lazyloaded" ||
          lower.starts_with("lazy-")) {
        return true;
      }
    }
    return false;
  }

  // What a reference in this document resolves against (DocumentBaseOf, the
  // rule every consumer of a scan shares): the
  // directory of its base URL and the host a hostless reference fetches from.
  // That is the base URL's host, as the browser resolves it: under a
  // cross-host <base href="https://cdn.example.com/assets/"> a root-relative
  // "/css/a.css" is the CDN's sheet. ResolvePath returns a root-relative
  // <base href="/sub/"> as it is, hostless, so that base keeps the page's
  // host; a protocol-relative "//cdn.example.com/" keeps its own.
  DocumentBase BaseOf() const { return DocumentBaseOf(page_url_, *result_); }

  // What identifies the image a `src` fetches, for comparing a stand-in with
  // its <noscript> copy: SheetIdentity's reduction, resolved
  // against the document's base (BaseOf: the <base href> included when it
  // has been seen), the scheme and fragment dropped, the host lowercased (the
  // base URL's for a reference without one: under a cross-host <base href>
  // a root-relative src fetches from the base's host, as the browser
  // resolves it). The query is kept: `/hero.jpg?v=2` is not known to be
  // `/hero.jpg`; percent-encoding and an explicit default port are compared
  // literally.
  std::string ImageIdentity(std::string_view src) const {
    const DocumentBase base = BaseOf();
    return SheetIdentity(base.dir, base.host, src);
  }

  // Stylesheet links in document order, resolved in EndDocument.
  enum class PendingKind : uint8_t {
    kApplies,       // a stylesheet link that applies to the document
    kNoscriptTwin,  // a stylesheet link inside <noscript> only
    kScriptLoaded,  // an author preload as=style with an onload handler
  };
  struct PendingStylesheet {
    StylesheetLink link;
    PendingKind kind;
  };
  std::vector<PendingStylesheet> pending_stylesheets_;
  // (index in stylesheet_sources, index in pending_stylesheets_) of each link
  // source queued as a stylesheet; resolved in EndDocument.
  std::vector<std::pair<size_t, size_t>> link_source_pending_;
  std::string page_url_;

  // Origin extraction for preconnect hints
  std::string page_origin_;
  std::unordered_set<std::string> seen_origins_;
  std::vector<OriginEntry> origin_entries_;
};

}  // namespace

DocumentBase DocumentBaseOf(std::string_view page_url,
                            const HtmlScanResult& scan,
                            std::string_view page_host,
                            std::string_view page_scheme) {
  // The page's own host and scheme: the URL's when it has them, else what the
  // caller knows (a cache-normalized page URL is the path alone).
  std::string own_host(UrlHostname(page_url));
  if (own_host.empty()) own_host = std::string(page_host);
  std::string own_scheme(SchemeOf(page_url));
  if (own_scheme.empty()) own_scheme = std::string(page_scheme);

  DocumentBase base;
  base.url = std::string(page_url);
  if (scan.has_base_href) {
    base.url = ResolvePath(DirectoryOf(page_url), scan.base_href);
  }
  base.dir = DirectoryOf(base.url);
  base.scheme = std::string(SchemeOf(base.url));
  if (base.scheme.empty()) base.scheme = own_scheme;
  // A protocol-relative base ("//cdn.example.com/") names its host without a
  // scheme; UrlHostname needs one to find it.
  std::string base_host(UrlHostname(base.url));
  if (base_host.empty() && base.url.starts_with("//")) {
    base_host = std::string(UrlHostname(absl::StrCat("x:", base.url)));
  }
  net_instaweb::AsciiToLowerInPlace(base_host);
  net_instaweb::AsciiToLowerInPlace(own_host);
  if (base_host.empty()) {
    base.host = std::move(own_host);
    return base;
  }
  base.cross_host =
      !own_host.empty() && HostWithoutDefaultPort(base_host, base.scheme) !=
                               HostWithoutDefaultPort(own_host, own_scheme);
  base.host = std::move(base_host);
  return base;
}

// True when `href` starts with a URL scheme ("https:", "data:", "javascript:",
// "mailto:"): such a reference is absolute and resolves against nothing.
bool HasScheme(std::string_view href) {
  if (href.empty() ||
      !absl::ascii_isalpha(static_cast<unsigned char>(href[0]))) {
    return false;
  }
  for (size_t i = 1; i < href.size(); ++i) {
    const char c = href[i];
    if (c == ':') return true;
    if (!absl::ascii_isalnum(static_cast<unsigned char>(c)) && c != '+' &&
        c != '-' && c != '.') {
      return false;
    }
  }
  return false;
}

std::string ResolveAgainstBase(const DocumentBase& base,
                               std::string_view href) {
  href = absl::StripAsciiWhitespace(href);
  // A reference with a scheme is already absolute: an https URL as it is, and
  // a data: or javascript: href is not a location on any host (ResolvePath
  // would prefix the base directory, and under a cross-host base the host).
  if (HasScheme(href)) return std::string(href);
  std::string resolved = ResolvePath(base.dir, href);
  if (resolved.find("://") != std::string::npos) return resolved;
  if (resolved.starts_with("//")) {
    // A protocol-relative reference takes the document's scheme, <base> or
    // not; without one the cache key could not be split into host and path
    // (the literal "//host/path" on the page host is a key nginx never
    // stores, so before the document-base rule such a sheet always counted as
    // missing).
    return base.scheme.empty() ? resolved
                               : absl::StrCat(base.scheme, ":", resolved);
  }
  if (!base.cross_host || base.host.empty()) return resolved;
  return absl::StrCat(base.scheme.empty() ? "https" : base.scheme, "://",
                      base.host, resolved.starts_with("/") ? "" : "/",
                      resolved);
}

bool IsInvisibleElement(const net_instaweb::HtmlElement& element) {
  using net_instaweb::HtmlName;

  // Tiny explicit dimensions: tracking beacons and tracking iframes declare
  // 0/1 px attributes.  Either axis at <= 1 px suffices — no visible
  // element declares it.
  // Surrounding ASCII whitespace is trimmed first: browsers parse
  // dimension attributes leniently, so width=" 1" is a 1px beacon too.
  auto is_tiny_dimension = [](const net_instaweb::HtmlElement::Attribute* a) {
    if (a == nullptr) return false;
    const char* value = a->DecodedValueOrNull();
    if (value == nullptr) return false;
    std::string_view v = absl::StripAsciiWhitespace(value);
    if (v.empty()) return false;
    int parsed = 0;
    for (char c : v) {
      if (c < '0' || c > '9') return false;  // not a plain pixel integer
      parsed = parsed * 10 + (c - '0');
      if (parsed > 1) return false;
    }
    return true;  // "0", "1" (and leading-zero forms thereof)
  };
  if (is_tiny_dimension(element.FindAttribute(HtmlName::kWidth)) ||
      is_tiny_dimension(element.FindAttribute(HtmlName::kHeight))) {
    return true;
  }

  // The hidden attribute removes the element from rendering entirely.
  if (element.FindAttribute("hidden") != nullptr) return true;

  // Inline style that removes the element from rendering.  Whitespace is
  // stripped before matching so "display : none" variants are caught.
  const char* style = element.AttributeValue(HtmlName::kStyle);
  if (style != nullptr) {
    std::string normalized;
    for (const char* p = style; *p != '\0'; ++p) {
      char c = *p;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
      normalized.push_back(net_instaweb::LowerChar(c));
    }
    if (normalized.find("display:none") != std::string::npos ||
        normalized.find("visibility:hidden") != std::string::npos) {
      return true;
    }
  }

  return false;
}

namespace {

enum class WidthFit : std::uint8_t { kAbsent, kFits, kMayExceed };

// Reads one CSS length the markup states as a width (an attribute value or an
// inline declaration value) against kNarrowestPhoneViewportPx.
WidthFit ReadMarkupWidth(std::string_view value) {
  value = absl::StripAsciiWhitespace(value);
  if (absl::EndsWithIgnoreCase(value, "!important")) {
    value = absl::StripAsciiWhitespace(
        value.substr(0, value.size() - std::string_view("!important").size()));
  }
  if (value.empty()) return WidthFit::kAbsent;
  size_t i = 0;
  double number = 0;
  bool digits = false;
  while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
    number = number * 10 + (value[i] - '0');
    digits = true;
    ++i;
  }
  if (i < value.size() && value[i] == '.') {
    double scale = 0.1;
    for (++i; i < value.size() && value[i] >= '0' && value[i] <= '9'; ++i) {
      number += (value[i] - '0') * scale;
      scale /= 10;
      digits = true;
    }
  }
  // `auto`, `min-content`, calc() and the like: the element decides, which is
  // exactly the case this exists for.
  if (!digits) return WidthFit::kMayExceed;
  std::string_view unit = value.substr(i);
  double px = -1;
  if (unit.empty() || absl::EqualsIgnoreCase(unit, "px")) {
    px = number;
  } else if (absl::EqualsIgnoreCase(unit, "rem") ||
             absl::EqualsIgnoreCase(unit, "em")) {
    px = number * 16;
  } else if (unit == "%" || absl::EndsWithIgnoreCase(unit, "vw") ||
             absl::EqualsIgnoreCase(unit, "vmin")) {
    // Relative to the containing block or the viewport: at most the viewport
    // for anything up to 100.
    return number <= 100 ? WidthFit::kFits : WidthFit::kMayExceed;
  } else {
    return WidthFit::kMayExceed;
  }
  return px <= kNarrowestPhoneViewportPx ? WidthFit::kFits
                                         : WidthFit::kMayExceed;
}

// Reads a length the markup states in CSS px (an attribute value or an inline
// declaration value): a plain number, with or without `px`, as the HTML rules
// for parsing dimension values and a px length agree on. Anything else
// (`50%`, `auto`, `3em`, empty) is unknown: -1.
double ReadMarkupPx(std::string_view value) {
  value = absl::StripAsciiWhitespace(value);
  if (absl::EndsWithIgnoreCase(value, "!important")) {
    value = absl::StripAsciiWhitespace(
        value.substr(0, value.size() - std::string_view("!important").size()));
  }
  size_t i = 0;
  double number = 0;
  bool digits = false;
  while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
    number = number * 10 + (value[i] - '0');
    digits = true;
    ++i;
  }
  if (i < value.size() && value[i] == '.') {
    double scale = 0.1;
    for (++i; i < value.size() && value[i] >= '0' && value[i] <= '9'; ++i) {
      number += (value[i] - '0') * scale;
      scale /= 10;
      digits = true;
    }
  }
  if (!digits) return -1;
  std::string_view unit = value.substr(i);
  if (!unit.empty() && !absl::EqualsIgnoreCase(unit, "px")) return -1;
  return number;
}

}  // namespace

bool MayExceedPhoneViewport(const net_instaweb::HtmlElement& element) {
  using net_instaweb::HtmlName;
  const std::string_view tag = element.name_str();
  const bool is_audio = net_instaweb::StringCaseEqual(tag, "audio");
  if (!net_instaweb::StringCaseEqual(tag, "img") &&
      !net_instaweb::StringCaseEqual(tag, "svg") &&
      !net_instaweb::StringCaseEqual(tag, "iframe") &&
      !net_instaweb::StringCaseEqual(tag, "canvas") &&
      !net_instaweb::StringCaseEqual(tag, "video") &&
      !net_instaweb::StringCaseEqual(tag, "embed") &&
      !net_instaweb::StringCaseEqual(tag, "object") && !is_audio) {
    return false;
  }
  // An <audio> without controls renders nothing.
  if (is_audio && element.FindAttribute("controls") == nullptr) return false;
  const StyleScope scope = ScopeOf(&element);
  if (scope.noscript || scope.inert || scope.svg || scope.math) return false;

  WidthFit width = WidthFit::kAbsent;
  bool max_width_fits = false;
  const char* style = element.AttributeValue(HtmlName::kStyle);
  if (style != nullptr) {
    for (std::string_view decl : absl::StrSplit(style, ';')) {
      const size_t colon = decl.find(':');
      if (colon == std::string_view::npos) continue;
      const std::string_view name =
          absl::StripAsciiWhitespace(decl.substr(0, colon));
      const WidthFit read = ReadMarkupWidth(decl.substr(colon + 1));
      // Later declarations win; an empty one is ignored by the browser.
      if (absl::EqualsIgnoreCase(name, "width") && read != WidthFit::kAbsent) {
        width = read;
      } else if (absl::EqualsIgnoreCase(name, "max-width") &&
                 read != WidthFit::kAbsent) {
        max_width_fits = read == WidthFit::kFits;
      }
    }
  }
  if (max_width_fits) return false;
  if (width == WidthFit::kAbsent) {
    const char* attribute = element.AttributeValue(HtmlName::kWidth);
    if (attribute != nullptr) width = ReadMarkupWidth(attribute);
  }
  return width != WidthFit::kFits;
}

bool IsUnlikelyLcpImage(const net_instaweb::HtmlElement& element) {
  // An image the markup declares invisible cannot be the LCP element.
  return IsInvisibleElement(element);
}

bool HasLazySourceAttribute(const net_instaweb::HtmlElement& element) {
  for (auto it = element.attributes().begin(); it != element.attributes().end();
       ++it) {
    std::string_view name(it->name_str().data(), it->name_str().size());
    if (name.size() > 5 && net_instaweb::StringCaseStartsWith(name, "data-") &&
        (name.find("src") != std::string_view::npos ||
         net_instaweb::StringCaseEqual(name, "data-original") ||
         net_instaweb::StringCaseEqual(name, "data-lazy"))) {
      return true;
    }
  }
  return false;
}

bool IsLazyStandInSrc(std::string_view src) {
  // The file name: after the last '/', before any query or fragment.
  src = src.substr(0, src.find_first_of("?#"));
  const size_t slash = src.rfind('/');
  if (slash != std::string_view::npos) src.remove_prefix(slash + 1);
  // Without its extension, when there is a name in front of one.
  const size_t dot = src.rfind('.');
  if (dot != std::string_view::npos && dot > 0) src = src.substr(0, dot);
  if (src.empty()) return false;
  std::string stem(src);
  net_instaweb::AsciiToLowerInPlace(stem);
  static constexpr std::string_view kNames[] = {
      "blank",       "placeholder", "pixel", "spacer",
      "transparent", "loading",     "lazy",  "1x1"};
  // A number, or a `<n>x<m>` size.
  auto is_size = [](std::string_view token) {
    bool digits = false;
    bool x_seen = false;
    for (size_t i = 0; i < token.size(); ++i) {
      const char c = token[i];
      if (c >= '0' && c <= '9') {
        digits = true;
      } else if (c == 'x' && !x_seen && digits && i + 1 < token.size()) {
        x_seen = true;
        digits = false;
      } else {
        return false;
      }
    }
    return digits;
  };
  bool named = false;
  for (std::string_view token :
       absl::StrSplit(stem, absl::ByAnyChar("-_."), absl::SkipEmpty())) {
    // A trailing number is part of the word (`spacer2`, `pixel1`).
    std::string_view word = token;
    while (!word.empty() && word.back() >= '0' && word.back() <= '9') {
      word.remove_suffix(1);
    }
    bool known = false;
    for (std::string_view name : kNames) {
      if (token == name || word == name) {
        known = true;
        break;
      }
    }
    if (known) {
      named = true;
    } else if (!is_size(token)) {
      return false;
    }
  }
  return named;
}

bool IsSmallDeclaredImage(const net_instaweb::HtmlElement& element) {
  using net_instaweb::HtmlName;
  double width = -1;
  double height = -1;
  bool style_width = false;
  bool style_height = false;
  const char* style = element.AttributeValue(HtmlName::kStyle);
  if (style != nullptr) {
    for (std::string_view decl : absl::StrSplit(style, ';')) {
      const size_t colon = decl.find(':');
      if (colon == std::string_view::npos) continue;
      const std::string_view name =
          absl::StripAsciiWhitespace(decl.substr(0, colon));
      // Later declarations win; an inline declaration overrides the
      // attribute even when it is unreadable, which makes the size unknown.
      if (absl::EqualsIgnoreCase(name, "width")) {
        width = ReadMarkupPx(decl.substr(colon + 1));
        style_width = true;
      } else if (absl::EqualsIgnoreCase(name, "height")) {
        height = ReadMarkupPx(decl.substr(colon + 1));
        style_height = true;
      }
    }
  }
  if (!style_width) {
    const char* attribute = element.AttributeValue(HtmlName::kWidth);
    if (attribute != nullptr) width = ReadMarkupPx(attribute);
  }
  if (!style_height) {
    const char* attribute = element.AttributeValue(HtmlName::kHeight);
    if (attribute != nullptr) height = ReadMarkupPx(attribute);
  }
  if (width < 0 || height < 0) return false;
  return width * height < kSmallImageAreaPx2;
}

HtmlScanner::HtmlScanner() {
  // Initialize HTML keywords (idempotent)
  net_instaweb::HtmlKeywords::Init();
}

HtmlScanResult HtmlScanner::Scan(std::string_view url, std::string_view html) {
  HtmlScanResult result;

  // Create parser with a discarding message handler (the vendored
  // canonical kernel actively uses its MessageHandler).
  net_instaweb::NullMessageHandler message_handler;
  auto parser = std::make_unique<net_instaweb::HtmlParse>(&message_handler);

  // Create element collector filter
  auto collector = std::make_unique<ElementCollectorFilter>(&result);
  collector->set_page_url(url);
  parser->AddFilter(collector.get());

  // Note: We do NOT add HtmlWriterFilter - scanner doesn't produce output HTML

  // Parse the HTML
  if (!parser->StartParse(url)) {
    result.success = false;
    result.error_message = "Failed to start parsing: invalid URL";
    return result;
  }

  parser->ParseText(html);
  parser->FinishParse();

  result.success = true;
  return result;
}

}  // namespace pagespeed

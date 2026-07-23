/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

//         morlovich@google.com (Maksim Orlovich)
// See the header for overview.

#include "net/instaweb/rewriter/public/critical_selector_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>

#include "base/logging.h"
#include "net/instaweb/rewriter/public/critical_selector_finder.h"
#include "net/instaweb/rewriter/public/css_minify.h"
#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/rewriter/public/css_util.h"
#include "net/instaweb/rewriter/public/request_properties.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stl_util.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"
#include "third_party/css_parser/src/webutil/css/media.h"
#include "third_party/css_parser/src/webutil/css/parser.h"
#include "third_party/css_parser/src/webutil/css/selector.h"

namespace net_instaweb {

namespace {

// Helper that takes a std::vector-like collection, and compacts
// any null holes in it.
template <typename VectorType>
void Compact(VectorType* cl) {
  typename VectorType::iterator new_end =
      std::remove(cl->begin(), cl->end(),
                  static_cast<typename VectorType::value_type>(nullptr));
  cl->erase(new_end, cl->end());
}

// Cheaply identifies @keyframes at-rules (including vendor-prefixed forms
// like @-webkit-keyframes) held whole inside an unparsed region.
bool IsKeyframesRegion(StringPiece bytes) {
  TrimLeadingWhitespace(&bytes);
  if (!bytes.starts_with("@")) {
    return false;
  }
  bytes.remove_prefix(1);
  if (bytes.starts_with("-")) {
    // Skip a vendor prefix, e.g. the "-webkit-" in "@-webkit-keyframes".
    stringpiece_ssize_type end_of_prefix = bytes.find('-', 1);
    if (end_of_prefix == StringPiece::npos) {
      return false;
    }
    bytes.remove_prefix(end_of_prefix + 1);
  }
  return StringCaseStartsWith(bytes, "keyframes");
}

// Whether a GROUP_RULE node whose body filtered down to nothing may be
// dropped from the critical subset. Only @supports/@container preludes have
// no declaration side effects, so only they are dead bytes when empty.
// Everything else keeps the empty node: block-form @layer DECLARES its
// layer(s), and layer priority is first-occurrence declaration order (CSS
// Cascade 5, "Layer Ordering"), so dropping an emptied "@layer a{}" from
// "@layer a{...}@layer b{...}" would flip the inline subset's cascade order
// relative to the source and the deferred full copy. The prelude is verbatim
// source bytes, so an escape-obscured ident (e.g. "@\73 upports") evades the
// prefix test; the polarity is deliberately asymmetric because a mis-kept
// empty group costs a few bytes while a mis-dropped @layer declaration
// corrupts cascade order.
bool IsEmptyGroupDroppable(StringPiece group_prelude) {
  return StringCaseStartsWith(group_prelude, "@supports") ||
         StringCaseStartsWith(group_prelude, "@container");
}

// Whether c can continue a CSS identifier. Backslashes are deliberately
// opaque: an escape never matches the ASCII keywords scanned below, so an
// escape-obscured construct falls back to "keep verbatim", the conservative
// outcome everywhere these helpers are used.
bool IsCssIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// Consumes CSS whitespace and /* */ comments at the head of bytes.
void SkipCssSpaceAndComments(StringPiece* bytes) {
  while (!bytes->empty()) {
    char c = (*bytes)[0];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      bytes->remove_prefix(1);
    } else if (bytes->starts_with("/*")) {
      stringpiece_ssize_type end = bytes->find("*/", 2);
      if (end == StringPiece::npos) {
        return;  // Unterminated comment; leave the rest unscanned.
      }
      bytes->remove_prefix(end + 2);
    } else {
      return;
    }
  }
}

// Whether bytes starts with the (lowercase) keyword as a whole CSS
// identifier, case-insensitively; consumes it from the head on match.
bool ConsumeCssKeyword(StringPiece keyword, StringPiece* bytes) {
  if (!StringCaseStartsWith(*bytes, keyword)) {
    return false;
  }
  if (bytes->size() > keyword.size() &&
      IsCssIdentChar((*bytes)[keyword.size()])) {
    return false;  // A longer identifier that merely starts with keyword.
  }
  bytes->remove_prefix(keyword.size());
  return true;
}

// Skips a quoted string at the head of bytes (bytes[0] must be the quote),
// backslash-escape aware. Returns false when the string never terminates.
bool SkipCssString(StringPiece* bytes) {
  char quote = (*bytes)[0];
  stringpiece_ssize_type i = 1, n = bytes->size();
  while (i < n) {
    if ((*bytes)[i] == '\\') {
      i += 2;
    } else if ((*bytes)[i] == quote) {
      bytes->remove_prefix(i + 1);
      return true;
    } else {
      ++i;
    }
  }
  return false;
}

// Skips a balanced (...) block at the head of bytes (bytes[0] must be
// '('), honoring quoted strings and backslash escapes inside it. Returns
// false when no matching ')' exists.
bool SkipCssParens(StringPiece* bytes) {
  int depth = 0;
  stringpiece_ssize_type i = 0, n = bytes->size();
  while (i < n) {
    char c = (*bytes)[i];
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      // Parens inside a quoted string do not count towards balancing.
      char quote = c;
      for (++i; i < n && (*bytes)[i] != quote;
           i += ((*bytes)[i] == '\\' ? 2 : 1)) {
      }
      if (i >= n) {
        return false;  // Unterminated string.
      }
      ++i;  // Past the closing quote.
      continue;
    }
    if (c == '(') {
      ++depth;
    } else if (c == ')' && --depth == 0) {
      bytes->remove_prefix(i + 1);
      return true;
    }
    ++i;
  }
  return false;
}

// Whether the region bytes are a statement-form "@layer ...;" (a block
// form would be a GROUP_RULE node, never an UNPARSED_REGION).
bool IsLayerStatementRegion(StringPiece bytes) {
  TrimLeadingWhitespace(&bytes);
  if (!StringCaseStartsWith(bytes, "@layer")) {
    return false;
  }
  return bytes.size() == 6 || !IsCssIdentChar(bytes[6]);
}

// What the critical subset should do with a top-level UNPARSED_REGION,
// per ClassifyImportRegion().
enum ImportRegionDisposition : std::uint8_t {
  kNotImportRegion,         // Not recognizable as an @import: keep verbatim.
  kDropImportRegion,        // No declaration to retain, or an invalid import:
                            // drop like the imports() bucket.
  kKeepImportRegion,        // Conditioned import: keep verbatim so the browser
                            // evaluates the conditions itself.
  kRetainLayerDeclaration,  // Unconditioned named-layer import: rewrite to
                            // a statement-form "@layer <name>;" region.
};

// Classifies the verbatim bytes of a top-level UNPARSED_REGION as an
// @import statement per the CSS Cascade 5 production
//   @import [<url> | <string>] [layer | layer(<layer-name>)]?
//       <import-conditions>? ;
// and decides what the subset may do with it (see the enum above).
// *layer_name is set only for kRetainLayerDeclaration. An escape anywhere
// after the @import keyword bails to kNotImportRegion, so obscure bytes
// are kept, never misread.
ImportRegionDisposition ClassifyImportRegion(StringPiece bytes,
                                             GoogleString* layer_name) {
  layer_name->clear();
  TrimLeadingWhitespace(&bytes);
  if (!ConsumeCssKeyword("@import", &bytes)) {
    return kNotImportRegion;
  }
  if (bytes.find('\\') != StringPiece::npos) {
    return kNotImportRegion;
  }
  SkipCssSpaceAndComments(&bytes);
  // Skip the URL: url(...) or a quoted string. Anything else is not a
  // valid import, and no clause scan is safe without a known-good URL
  // token (a misread one could hide a "layer(" inside it), so stop at
  // "import with no retainable declaration".
  if (ConsumeCssKeyword("url", &bytes)) {
    if (bytes.empty() || bytes[0] != '(' || !SkipCssParens(&bytes)) {
      return kDropImportRegion;
    }
  } else if (!bytes.empty() && (bytes[0] == '"' || bytes[0] == '\'')) {
    if (!SkipCssString(&bytes)) {
      return kDropImportRegion;
    }
  } else {
    return kDropImportRegion;
  }
  SkipCssSpaceAndComments(&bytes);
  // The layer clause comes FIRST in the grammar production above, so
  // anything else here means the import declares no layer. In particular
  // a supports() BEFORE a layer clause is invalid CSS that browsers
  // ignore whole --- never retain a name from it.
  if (!ConsumeCssKeyword("layer", &bytes)) {
    return kDropImportRegion;
  }
  // Bare "layer" declares an anonymous layer (no statement form exists,
  // so there is no name to retain); "layer(<name>)" declares a named
  // layer. The '(' must immediately follow the keyword: with whitespace
  // in between, the source is the anonymous keyword plus a media
  // expression.
  if (bytes.empty() || bytes[0] != '(') {
    return kDropImportRegion;
  }
  bytes.remove_prefix(1);
  stringpiece_ssize_type close = bytes.find(')');
  if (close == StringPiece::npos) {
    return kDropImportRegion;  // Unbalanced: no name to read.
  }
  StringPiece name = bytes.substr(0, close);
  TrimWhitespace(&name);
  bool valid = !name.empty();
  for (int i = 0, n = name.size(); i < n; ++i) {
    if (name[i] != '.' && !IsCssIdentChar(name[i])) {
      valid = false;  // Not a single (possibly dotted) layer name.
      break;
    }
  }
  if (!valid) {
    return kDropImportRegion;
  }
  // Anything between the clause's ')' and the terminating ';' is an
  // import condition (a supports(...) clause or a media query list). The
  // layer declaration is subject to such conditions (CSS Cascade 5): a
  // false document-global condition means the layer contributes nothing
  // to layer order, so an unconditional "@layer <name>;" would
  // misdeclare. Keep the region verbatim; the browser evaluates the
  // import itself.
  bytes.remove_prefix(close + 1);
  SkipCssSpaceAndComments(&bytes);
  if (bytes.size() != 1 || bytes[0] != ';') {
    return kKeepImportRegion;
  }
  layer_name->assign(name.data(), name.size());
  return kRetainLayerDeclaration;
}

// Rewrites top-level @import UNPARSED_REGIONs for the critical subset: an
// unconditioned named-layer import becomes a statement-form
// "@layer <name>;" region in its original position (the import is a
// layer declaration, and layer priority is first-occurrence order ---
// CSS Cascade 5, "Layer Ordering"); a CONDITIONED import keeps its region
// verbatim so the browser evaluates the conditions; any other import
// region is dropped like the imports() bucket. Import regions carrying a
// media annotation (flattened out of a top-level @media) are dropped
// without a statement: nested @import is invalid CSS that browsers
// ignore entirely. Regions past the top of the sheet (a style rule,
// group rule, or other at-rule region preceded them) are likewise
// invalid imports and are dropped declaration-free; a top-level
// statement-form @layer is the one statement Cascade 5 allows to
// precede an @import, so it does not invalidate a following one --- but
// an annotated one, flattened out of a @media block, was wrapped in an
// at-rule and does. Regions the scanner cannot read stay verbatim,
// matching the keep-everything-unknown polarity of FilterStylesheet().
// One blind spot: the font_faces() bucket's source position is lost to
// this loop, so a @font-face before a layered import (an already-invalid
// sheet whose import no browser honors) cannot flip imports_valid here.
void RewriteLayeredImportRegions(Css::Stylesheet* stylesheet) {
  bool imports_valid = true;  // Nothing but statements/imports seen so far.
  Css::Rulesets& rulesets = stylesheet->mutable_rulesets();
  for (int i = 0, n = rulesets.size(); i < n; ++i) {
    Css::Ruleset* r = rulesets.at(i);
    if (r->type() == Css::Ruleset::RULESET ||
        r->type() == Css::Ruleset::GROUP_RULE) {
      imports_valid = false;
      continue;
    }
    CssStringPiece region_bytes =
        r->unparsed_region()->bytes_in_original_buffer();
    StringPiece bytes(region_bytes.data(), region_bytes.size());
    GoogleString layer_name;
    ImportRegionDisposition disposition =
        ClassifyImportRegion(bytes, &layer_name);
    if (disposition != kNotImportRegion && !r->media_queries().empty()) {
      disposition = kDropImportRegion;
    }
    if (disposition == kDropImportRegion ||
        (disposition == kRetainLayerDeclaration && !imports_valid)) {
      delete r;
      rulesets[i] = nullptr;
    } else if (disposition == kRetainLayerDeclaration) {
      r->mutable_unparsed_region()->set_bytes_in_original_buffer(
          StrCat("@layer ", layer_name, ";"));
    } else if (disposition == kNotImportRegion &&
               (!IsLayerStatementRegion(bytes) ||
                !r->media_queries().empty())) {
      imports_valid = false;
    }
    // kKeepImportRegion: leave the region untouched so the browser
    // evaluates the conditioned import itself.
  }
  Compact(&rulesets);
}

}  // namespace

const char CriticalSelectorFilter::kNoscriptStylesClass[] = "psa_add_styles";

// Wrap CSS elements to move them later in the document.
// A simple list of elements is insufficient because link tags and style tags
// are inserted different.
class CriticalSelectorFilter::CssElement {
 public:
  CssElement(HtmlParse* p, HtmlElement* e, bool inside_noscript)
      : html_parse_(p),
        element_(p->CloneElement(e)),
        inside_noscript_(inside_noscript) {}

  // HtmlParse deletes the element (regardless of whether it is inserted).
  virtual ~CssElement() {}

  virtual void AppendTo(HtmlElement* parent) const {
    html_parse_->AppendChild(parent, element_);
  }

  bool inside_noscript() const { return inside_noscript_; }

 protected:
  HtmlParse* html_parse_;
  HtmlElement* element_;
  bool inside_noscript_;

 private:
  CssElement(const CssElement&) = delete;
  CssElement& operator=(const CssElement&) = delete;
};

// Wrap CSS style blocks to move them later in the document.
class CriticalSelectorFilter::CssStyleElement
    : public CriticalSelectorFilter::CssElement {
 public:
  CssStyleElement(HtmlParse* p, HtmlElement* e, bool inside_noscript)
      : CssElement(p, e, inside_noscript) {}
  ~CssStyleElement() override {}

  // Call before InsertBeforeCurrent.
  void AppendCharactersNode(HtmlCharactersNode* characters_node) {
    characters_nodes_.push_back(
        html_parse_->NewCharactersNode(nullptr, characters_node->contents()));
  }

  void AppendTo(HtmlElement* parent) const override {
    HtmlElement* element = element_;
    CssElement::AppendTo(parent);
    for (CharactersNodeVector::const_iterator it = characters_nodes_.begin(),
                                              end = characters_nodes_.end();
         it != end; ++it) {
      html_parse_->AppendChild(element, *it);
    }
  }

 protected:
  using CharactersNodeVector = std::vector<HtmlCharactersNode*>;
  CharactersNodeVector characters_nodes_;

 private:
  CssStyleElement(const CssStyleElement&) = delete;
  CssStyleElement& operator=(const CssStyleElement&) = delete;
};

// Wrap CSS related elements so they can be moved later in the document.
CriticalSelectorFilter::CriticalSelectorFilter(RewriteDriver* driver)
    : CssSummarizerBase(driver),
      saw_end_document_(false),
      any_rendered_(false) {}

CriticalSelectorFilter::~CriticalSelectorFilter() {}

void CriticalSelectorFilter::Summarize(Css::Stylesheet* stylesheet,
                                       GoogleString* out) const {
  // The critical subset is inlined into the page, where an @import would
  // still trigger a synchronous, render-blocking fetch. No rule in the
  // critical subset can depend on it: imported files' selectors were never
  // beacon candidates. The deferred full copy of the CSS retains the import.
  // Top level only: group bodies never contain imports (rejected at parse
  // time), so FilterStylesheet() need not drop them on recursion.
  //
  // The one bucketed import form with a cascade side effect is
  // "@import url(x) layer;", which the parser models with "layer" as its
  // media type. That declares an ANONYMOUS layer, which has no statement
  // form, and removing one occurrence from the layer sequence cannot
  // reorder the remaining ones --- the drop is cascade-neutral.
  STLDeleteElements(&stylesheet->mutable_imports());

  // A NAMED-layer import ("@import url(x) layer(theme);") never reaches the
  // bucket above: "layer(...)" is not a media query, so preservation mode
  // keeps the whole statement verbatim as an UNPARSED_REGION in the ordered
  // rulesets sequence --- the same representation statement-form "@layer a;"
  // rides. Retain its layer declaration as exactly such a statement (and
  // drop import regions that declare no named layer): kept whole, the
  // region would leak the render-blocking fetch into the inline subset;
  // dropped whole, it would take the declaration with it and could flip
  // first-occurrence layer order against the deferred full copy. Imports
  // with conditions after the layer clause keep their region verbatim
  // instead: the declaration is subject to the conditions, which the
  // browser must evaluate per visitor.
  RewriteLayeredImportRegions(stylesheet);

  FilterStylesheet(stylesheet);

  // Serialize out the remaining subset.
  StringWriter writer(out);
  NullMessageHandler handler;
  CssMinify::Stylesheet(*stylesheet, &writer, &handler);
}

void CriticalSelectorFilter::FilterStylesheet(
    Css::Stylesheet* stylesheet) const {
  for (int ruleset_index = 0, num_rulesets = stylesheet->rulesets().size();
       ruleset_index < num_rulesets; ++ruleset_index) {
    Css::Ruleset* r = stylesheet->mutable_rulesets().at(ruleset_index);
    if (r->type() == Css::Ruleset::GROUP_RULE) {
      // Media filtering identical to the RULESET path below: the group
      // node's annotation (from an enclosing top-level @media) gates its
      // whole body, and body rulesets may carry empty annotations of their
      // own, so it must be applied here rather than left to the recursion.
      bool any_media_apply = r->media_queries().empty();
      for (int mediaquery_index = 0, num_mediaquery = r->media_queries().size();
           mediaquery_index < num_mediaquery; ++mediaquery_index) {
        Css::MediaQuery* mq = r->mutable_media_queries().at(mediaquery_index);
        if (css_util::CanMediaAffectScreen(mq->ToString())) {
          any_media_apply = true;
        } else {
          delete mq;
          r->mutable_media_queries()[mediaquery_index] = nullptr;
        }
      }
      if (!any_media_apply) {
        // Every query is definitely non-screen (CanMediaAffectScreen treats
        // unknown/raw forms as screen-affecting), e.g.
        // "@media print{@layer a{...}}". The whole node is deleted, @layer
        // included: layer order is determined by each layer name's first
        // occurrence, and a @layer inside a conditional rule whose condition
        // does not match takes no effect and so contributes no occurrence
        // (CSS Cascade 5, "Layer Ordering",
        // https://www.w3.org/TR/css-cascade-5/#layer-ordering — the spec's
        // note advising up-front @layer statements exists because of this).
        // The spec's element-sensitive exception (layers inside @container
        // contribute regardless of the condition) never applies here: only
        // the node's @media annotation — a document-global condition — is
        // evaluated, and an emptied inner @layer keeps its @container
        // parent's body non-empty via IsEmptyGroupDroppable.
        // On screen the source declared nothing, so the subset declaring
        // nothing is byte-identical in effect; print semantics are restored
        // by the deferred full copy.
        delete r;
        stylesheet->mutable_rulesets()[ruleset_index] = nullptr;
        continue;
      }
      Compact(&r->mutable_media_queries());
      // Summarize() handled imports on the top level only; the parser
      // rejects @import inside group bodies, so none can survive to here.
      DCHECK(r->group_body().imports().empty());
      FilterStylesheet(r->mutable_group_body());
      // The recursion filtered body rulesets by media + selectors, dropped
      // body @keyframes regions, kept statement-form "@layer x, y;" regions
      // (they declare layers), and left body font_faces() untouched; a
      // font-face-only body therefore keeps its group. Depth is bounded by
      // the parser's group-nesting cap.
      if (r->group_body().rulesets().empty() &&
          r->group_body().font_faces().empty() &&
          IsEmptyGroupDroppable(r->group_prelude())) {
        delete r;
        stylesheet->mutable_rulesets()[ruleset_index] = nullptr;
      }
      // Survivors are re-wrapped under the verbatim prelude and the
      // compacted media annotation at serialization; the browser
      // re-evaluates the group condition, so a rule kept under a false
      // condition stays inert exactly as in the full sheet.
      continue;
    }
    if (r->type() != Css::Ruleset::RULESET) {
      // Only plain rulesets are filtered by selector below (the getters
      // CHECK on type). The keyframes-drop keys on UNPARSED_REGION:
      // @keyframes end up there whole under preservation mode; animations
      // don't affect first paint and their keyframe lists can be large, so
      // drop them from the critical subset (the deferred full copy retains
      // them) — at every nesting level, since this helper also runs on
      // group bodies. Every other unparsed region is kept unaltered to be
      // conservative — notably statement-form "@layer a, b;", whose layer
      // declarations are load-bearing for cascade order.
      if (r->type() == Css::Ruleset::UNPARSED_REGION) {
        CssStringPiece region_bytes =
            r->unparsed_region()->bytes_in_original_buffer();
        if (IsKeyframesRegion(
                StringPiece(region_bytes.data(), region_bytes.size()))) {
          delete r;
          stylesheet->mutable_rulesets()[ruleset_index] = nullptr;
        }
      }
      continue;
    }

    // TODO(morlovich): This does a lot of repeated work as the same media
    // entries are repeated for tons of rulesets.
    // TODO(morlovich): It's silly to serialize this, we should work directly
    // off AST once we have decision procedure on that.

    bool any_media_apply = r->media_queries().empty();
    for (int mediaquery_index = 0, num_mediaquery = r->media_queries().size();
         mediaquery_index < num_mediaquery; ++mediaquery_index) {
      Css::MediaQuery* mq = r->mutable_media_queries().at(mediaquery_index);
      if (css_util::CanMediaAffectScreen(mq->ToString())) {
        any_media_apply = true;
      } else {
        delete mq;
        r->mutable_media_queries()[mediaquery_index] = nullptr;
      }
    }

    bool any_selectors_apply = false;
    if (any_media_apply) {
      // See which of the selectors for given declaration apply.
      // Note that in some partial parse errors we will get 0 selectors here,
      // in which case we retain things to be conservative.
      any_selectors_apply = r->selectors().empty();
      for (int selector_index = 0, num_selectors = r->selectors().size();
           selector_index < num_selectors; ++selector_index) {
        Css::Selector* s = r->mutable_selectors().at(selector_index);
        GoogleString portion_to_compare = css_util::JsDetectableSelector(*s);
        if (portion_to_compare.empty() ||
            critical_selectors_.find(portion_to_compare) !=
                critical_selectors_.end()) {
          any_selectors_apply = true;
        } else {
          delete s;
          r->mutable_selectors()[selector_index] = nullptr;
        }
      }
    }

    if (any_selectors_apply && any_media_apply) {
      // Just remove the irrelevant selectors & media
      Compact(&r->mutable_selectors());
      Compact(&r->mutable_media_queries());
    } else {
      // Remove the entire production
      delete r;
      stylesheet->mutable_rulesets()[ruleset_index] = nullptr;
    }
  }
  Compact(&stylesheet->mutable_rulesets());
}

void CriticalSelectorFilter::RenderSummary(int pos, HtmlElement* element,
                                           HtmlCharactersNode* char_node,
                                           bool* is_element_deleted) {
  RememberFullCss(pos, element, char_node);

  const SummaryInfo& summary = GetSummaryForStyle(pos);
  DCHECK_EQ(kSummaryOk, summary.state);

  // If we're inlining an external CSS file, make sure to adjust the URLs
  // inside to the new base.
  const GoogleString* css_to_use = &summary.data;
  GoogleString resolved_css;
  if (summary.is_external) {
    StringWriter writer(&resolved_css);
    GoogleUrl input_css_base(summary.base);
    if (driver()->ResolveCssUrls(
            input_css_base, driver()->base_url().Spec(), summary.data, &writer,
            driver()->message_handler()) == RewriteDriver::kSuccess) {
      css_to_use = &resolved_css;
    }
  }

  // Inlining bytes whose charset differs from the page's can garble them
  // (e.g. non-UTF-8 content: strings) --- the equivalent of
  // CssInlineFilter::ShouldInline()'s charset check. A pure-ASCII critical
  // subset is charset-agnostic, so it is still safe. Otherwise leave this
  // stylesheet alone; as with WillNotRenderSummary, the full CSS was
  // remembered above so the page stays intact.
  //
  // An empty summary.charset means the stylesheet declared no charset (no
  // Content-Type charset, @charset, BOM, or charset attribute). We cannot
  // assume it matches the page, so treat unknown as "differs" and apply the
  // same non-ASCII bail-out --- otherwise mismatched bytes would be inlined
  // and garbled. CssInlineFilter avoids this by defaulting the charset before
  // comparing; here we bail whenever the charset is unknown or differs.
  if (summary.is_external &&
      (summary.charset.empty() ||
       !StringCaseEqual(driver()->containing_charset(), summary.charset))) {
    bool has_non_ascii = false;
    for (int i = 0, n = css_to_use->size(); i < n; ++i) {
      if (static_cast<unsigned char>((*css_to_use)[i]) >= 0x80) {
        has_non_ascii = true;
        break;
      }
    }
    if (has_non_ascii) {
      return;
    }
  }

  // Security: the CSS minifier decodes hex escapes (e.g. "\3C" -> '<') and
  // Css::EscapeString does not re-escape '<', '>' or '/', so crafted CSS
  // (e.g. content:"\3C/style\3E...") can serialize to a literal "</style>"
  // that would break out of the inline <style> element (XSS). Mirror
  // CssInlineFilter::HasClosingStyleTag: if the critical subset contains a
  // closing style tag, skip inlining it. RememberFullCss() above already
  // preserved the full CSS so the page stays intact.
  if (FindIgnoreCase(*css_to_use, "</style") != StringPiece::npos) {
    return;
  }

  // Update the DOM --- either an existing style element, or replace link
  // with style.
  if (char_node != nullptr) {
    // Note: This depends upon all previous filters also mutating the contents
    // of the original Characters Node. If any previous filters replaces the
    // Characters Node with another one or makes some other change, this node
    // will be out of date and the update will not do anything.
    // TODO(sligocki): We should use a non-trivial ResourceSlot to update this
    // instead so that it is not so delicate.
    *char_node->mutable_contents() = *css_to_use;
  } else {
    HtmlElement* style_element =
        driver()->NewElement(nullptr, HtmlName::kStyle);
    // Carry over attributes the page may depend on: id (stylesheet toggling
    // by script), title (stylesheet-set semantics), and data-*. media is
    // reconstructed below; the link-specific attributes (href, rel, type,
    // charset) don't apply to a style element.
    const HtmlElement::AttributeList& link_attrs = element->attributes();
    for (HtmlElement::AttributeConstIterator i(link_attrs.begin());
         i != link_attrs.end(); ++i) {
      const HtmlElement::Attribute& attr = *i;
      if (attr.keyword() == HtmlName::kId ||
          attr.keyword() == HtmlName::kTitle ||
          StringCaseStartsWith(attr.name_str(), "data-")) {
        style_element->AddAttribute(attr);
      }
    }
    driver()->InsertNodeBeforeNode(element, style_element);

    HtmlCharactersNode* content =
        driver()->NewCharactersNode(style_element, *css_to_use);
    driver()->AppendChild(style_element, content);
    *is_element_deleted = driver()->DeleteNode(element);
    element = style_element;
  }

  // Update the media attribute to just the media that's relevant to screen.
  StringVector all_media;
  css_util::VectorizeMediaAttribute(summary.media_from_html, &all_media);

  element->DeleteAttribute(HtmlName::kMedia);
  bool drop_entire_element = false;
  if (css_to_use->empty()) {  // NOLINT(bugprone-branch-clone)
    // Don't keep empty blocks around.
    drop_entire_element = true;
  } else if (summary.is_inside_noscript) {
    // Optimize summary version for scriptable environment, since noscript
    // environment will eagerly load the whole CSS anyway at the foot of the
    // page.
    drop_entire_element = true;
  } else if (summary.is_external &&
             CssTagScanner::IsAlternateStylesheet(summary.rel)) {
    // Likewise drop alternate stylesheets, they're non-critical.
    drop_entire_element = true;
  } else if (!all_media.empty()) {
    StringVector relevant_media;
    for (int i = 0, n = all_media.size(); i < n; ++i) {
      const GoogleString& medium = all_media[i];
      if (css_util::CanMediaAffectScreen(medium)) {
        relevant_media.push_back(medium);
      }
    }

    if (!relevant_media.empty()) {
      driver()->AddAttribute(element, HtmlName::kMedia,
                             css_util::StringifyMediaVector(relevant_media));
    } else {
      // None of the media applied to the screen, so remove the entire element.
      drop_entire_element = true;
    }
  }

  if (drop_entire_element) {
    driver()->DeleteNode(element);
  }

  // We've altered the CSS, so we should generate code to load the entire thing.
  // TODO(morlovich): Check if we actually dropped something?
  any_rendered_ = true;
}

void CriticalSelectorFilter::WillNotRenderSummary(
    int pos, HtmlElement* element, HtmlCharactersNode* char_node) {
  RememberFullCss(pos, element, char_node);
}

GoogleString CriticalSelectorFilter::CacheKeySuffix() const {
  return cache_key_suffix_;
}

void CriticalSelectorFilter::StartDocumentImpl() {
  CssSummarizerBase::StartDocumentImpl();
  ServerContext* context = driver()->server_context();

  // Read critical selector info from pcache.
  critical_selectors_ =
      context->critical_selector_finder()->GetCriticalSelectors(driver());

  // Compute corresponding cache key suffix
  GoogleString all_selectors = JoinCollection(critical_selectors_, ",");
  cache_key_suffix_ = context->lock_hasher()->Hash(all_selectors);

  // Clear state between re-uses / check to make sure we wrapped up properly.
  DCHECK(css_elements_.empty());
  saw_end_document_ = false;
  any_rendered_ = false;
}

void CriticalSelectorFilter::EndDocument() {
  CssSummarizerBase::EndDocument();

  saw_end_document_ = true;
}

void CriticalSelectorFilter::RenderDone() {
  CssSummarizerBase::RenderDone();

  // Only do this on very last flush window.
  if (!saw_end_document_) {
    return;
  }

  if (!css_elements_.empty() && any_rendered_) {
    HtmlElement* noscript_element = nullptr;
    Compact(&css_elements_);
    for (int i = 0, n = css_elements_.size(); i < n; ++i) {
      // Insert the full CSS, but hide all the style, link tags inside noscript
      // blocks so that look-ahead parser cannot find them; and mark the
      // portions that were visible to scripting-aware browser with
      // class = psa_add_styles.
      //
      // If the browser has scripting off, it will therefore read everything,
      // including portions of original CSS that were in noscript block.
      //
      // If the browser has scripting on, the parser will not do anything, but
      // we will add a loader script which will load things with
      // class = psa_add_styles (thus skipping over things that were originally
      // inside noscript).
      if (i == 0 || (css_elements_[i]->inside_noscript() !=
                     css_elements_[i - 1]->inside_noscript())) {
        noscript_element = driver()->NewElement(nullptr, HtmlName::kNoscript);
        if (!css_elements_[i]->inside_noscript()) {
          driver()->AddAttribute(noscript_element, HtmlName::kClass,
                                 kNoscriptStylesClass);
        }
        InsertNodeAtBodyEnd(noscript_element);
      }
      css_elements_[i]->AppendTo(noscript_element);
    }

    // The CriticalCssLoader bootstrap below is an inline <script>; a script-src
    // policy without 'unsafe-inline' would make the browser block it, stranding
    // the non-critical CSS we just moved into the <noscript> blocks. Mirror the
    // sibling critical_css_beacon_filter guard and never emit a loader the CSP
    // will block. The authoritative gate is PolicyPermitsRendering() (which now
    // also requires inline script), so with a forbidding policy we don't get
    // here at all; this is a defensive backstop. The ideal is for that single
    // enable decision to remain the only place this is expressed.
    if (!CspPermitsInlineScript()) {
      STLDeleteElements(&css_elements_);
      return;
    }

    HtmlElement* script = driver()->NewElement(nullptr, HtmlName::kScript);
    driver()->AddAttribute(script, HtmlName::kDataPagespeedNoDefer,
                           StringPiece());
    InsertNodeAtBodyEnd(script);
    GoogleString js =
        driver()->server_context()->static_asset_manager()->GetAsset(
            StaticAssetEnum::CRITICAL_CSS_LOADER_JS, driver()->options());
    if (!driver()
             ->options()
             ->test_only_prioritize_critical_css_dont_apply_original_css()) {
      StrAppend(&js, "pagespeed.CriticalCssLoader.Run();");
    }
    AddJsToElement(js, script);
  }

  STLDeleteElements(&css_elements_);
}

void CriticalSelectorFilter::DetermineEnabled(GoogleString* disabled_reason) {
  // We shouldn't do anything if there is no information on critical selectors
  // in the property cache. Unfortunately, we also cannot run safely in case of
  // IE, since we do not understand IE conditional comments well enough to
  // replicate their behavior in the load-everything section.
  const StringSet& critical_selectors = driver()
                                            ->server_context()
                                            ->critical_selector_finder()
                                            ->GetCriticalSelectors(driver());
  bool ua_supports_critical_css =
      driver()->request_properties()->SupportsCriticalCss();
  bool can_run = ua_supports_critical_css && !critical_selectors.empty();
  driver()->log_record()->LogRewriterHtmlStatus(
      RewriteOptions::FilterId(RewriteOptions::kPrioritizeCriticalCss),
      (can_run ? RewriterHtmlApplication::ACTIVE
               : (ua_supports_critical_css
                      ? RewriterHtmlApplication::PROPERTY_CACHE_MISS
                      : RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED)));

  if (!can_run) {
    if (!ua_supports_critical_css) {
      *disabled_reason = "User agent not supported";
    } else {
      *disabled_reason = "No critical selector info in cache";
    }
  }

  set_is_enabled(can_run);
}

void CriticalSelectorFilter::RememberFullCss(int pos, HtmlElement* element,
                                             HtmlCharactersNode* char_node) {
  // Deep copy[1] into the css_elements_ array the CSS as optimized by all the
  // filters that ran before us and rendered their results, so that we can
  // emit it accurately at end, as a lazy-load sequence.
  // [1] We need a deep copy since some of the DOM data will get freed up at the
  //     end of each flush window.
  if (static_cast<size_t>(pos) >= css_elements_.size()) {
    css_elements_.resize(pos + 1);
  }
  bool noscript = GetSummaryForStyle(pos).is_inside_noscript;
  CssElement* save = nullptr;
  if (char_node != nullptr) {
    CssStyleElement* save_inline =
        new CssStyleElement(driver(), element, noscript);
    save_inline->AppendCharactersNode(char_node);
    save = save_inline;
  } else {
    save = new CssElement(driver(), element, noscript);
  }
  css_elements_[pos] = save;
}

}  // namespace net_instaweb

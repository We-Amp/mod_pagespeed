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

// Filters an @media annotation down to the queries that can affect the
// screen: a definitely-non-screen query is deleted and its slot nulled,
// leaving holes for the caller to Compact() (or to discard with the node).
// The vector must not already contain null holes. Returns whether any query
// still applies — true for an empty annotation, which is unconditional.
// CanMediaAffectScreen() treats unknown/raw forms (MQ4 range syntax,
// partial parses) as screen-affecting, so the polarity is conservative:
// only definitely-non-screen media is dropped.
//
// Shared by both arms of FilterStylesheet() — a GROUP_RULE node's annotation
// (flattened out of an enclosing top-level @media) is filtered exactly like a
// RULESET's; only what the caller does with the verdict differs.
bool FilterMediaQueriesForScreen(Css::MediaQueries* media_queries) {
  bool any_media_apply = media_queries->empty();
  for (int mediaquery_index = 0, num_mediaquery = media_queries->size();
       mediaquery_index < num_mediaquery; ++mediaquery_index) {
    Css::MediaQuery* mq = media_queries->at(mediaquery_index);
    if (css_util::CanMediaAffectScreen(mq->ToString())) {
      any_media_apply = true;
    } else {
      delete mq;
      (*media_queries)[mediaquery_index] = nullptr;
    }
  }
  return any_media_apply;
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

// The summary of a stylesheet that must not be deferred: its <link> is left
// exactly as it is. The summarizer cannot produce this text for a stylesheet
// that has anything else in it.
const char kKeepBlockingSummary[] = "@pagespeed-keep-stylesheet-blocking;";

// A stylesheet at least this large (minified) keeps its blocking link when
// the inline subset would be kSubsetShareNumerator/kSubsetShareDenominator
// of it or more: nearly the whole file would be sent inline and then again
// as a file.
const size_t kMinBytesToWeighSubset = 4096;
const size_t kSubsetShareNumerator = 4;
const size_t kSubsetShareDenominator = 5;

// Whether the stylesheet imports another one in a way a browser acts on: a
// plain, anonymous-layer or named-layer @import at the top of the sheet.
// The rules of an imported file are never in the inline subset (their
// selectors are not reported on), so once the link is deferred they would
// be in nothing that blocks rendering.
//
// Not counted: an import nested in a media block or placed after a rule
// (browsers ignore both), and a layered import with conditions, which stays
// verbatim in the inline subset and so still blocks there. An import whose
// form this scan cannot make sense of counts: when in doubt the stylesheet
// keeps its blocking link.
bool HasLiveImport(const Css::Stylesheet& stylesheet) {
  if (!stylesheet.imports().empty()) {
    return true;
  }
  bool imports_valid = true;  // Nothing but statements/imports seen so far.
  const Css::Rulesets& rulesets = stylesheet.rulesets();
  for (int i = 0, n = rulesets.size(); i < n; ++i) {
    const Css::Ruleset* r = rulesets.at(i);
    if (r->type() != Css::Ruleset::UNPARSED_REGION) {
      return false;  // An import past a rule or group is not honoured.
    }
    CssStringPiece region_bytes =
        r->unparsed_region()->bytes_in_original_buffer();
    StringPiece bytes(region_bytes.data(), region_bytes.size());
    GoogleString layer_name;
    ImportRegionDisposition disposition =
        ClassifyImportRegion(bytes, &layer_name);
    if (disposition == kNotImportRegion) {
      if (!IsLayerStatementRegion(bytes) || !r->media_queries().empty()) {
        imports_valid = false;
      }
      continue;
    }
    if (!imports_valid || !r->media_queries().empty()) {
      continue;
    }
    if (disposition != kKeepImportRegion) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char CriticalSelectorFilter::kDeferredCssAttribute[] =
    "data-pagespeed-deferred-css";

CriticalSelectorFilter::CriticalSelectorFilter(RewriteDriver* driver)
    : CssSummarizerBase(driver),
      keep_unknown_selectors_(true),
      loader_inserted_(false) {}

CriticalSelectorFilter::~CriticalSelectorFilter() {}

void CriticalSelectorFilter::Summarize(Css::Stylesheet* stylesheet,
                                       GoogleString* out) const {
  // A stylesheet that still imports another one keeps its blocking link:
  // the imported rules would otherwise be in neither the inline subset nor
  // a request that blocks rendering. (Imports the server could flatten are
  // gone by now; what is left could not be fetched or was too large.)
  if (HasLiveImport(*stylesheet)) {
    *out = kKeepBlockingSummary;
    return;
  }

  NullMessageHandler handler;
  GoogleString whole;
  {
    StringWriter whole_writer(&whole);
    CssMinify::Stylesheet(*stylesheet, &whole_writer, &handler);
  }

  // What is left of imports here is not honoured by browsers (nested in a
  // media block, or placed after a rule) or carries conditions. The first
  // kind is dropped; a conditioned layered import keeps its region verbatim,
  // so the browser evaluates the conditions itself and the import blocks
  // from inside the inline subset.
  RewriteLayeredImportRegions(stylesheet);

  FilterStylesheet(stylesheet);

  // Serialize out the remaining subset.
  StringWriter writer(out);
  CssMinify::Stylesheet(*stylesheet, &writer, &handler);

  if (whole.size() >= kMinBytesToWeighSubset &&
      out->size() * kSubsetShareDenominator >=
          whole.size() * kSubsetShareNumerator) {
    *out = kKeepBlockingSummary;
  }
}

void CriticalSelectorFilter::FilterStylesheet(
    Css::Stylesheet* stylesheet) const {
  for (int ruleset_index = 0, num_rulesets = stylesheet->rulesets().size();
       ruleset_index < num_rulesets; ++ruleset_index) {
    Css::Ruleset* r = stylesheet->mutable_rulesets().at(ruleset_index);
    if (r->type() == Css::Ruleset::GROUP_RULE) {
      // The group node's annotation (from an enclosing top-level @media)
      // gates its whole body, and body rulesets may carry empty annotations
      // of their own, so filtering must happen here rather than be left to
      // the recursion.
      bool any_media_apply =
          FilterMediaQueriesForScreen(&r->mutable_media_queries());
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
      // The recursion filtered body rulesets by media + selectors, kept
      // every verbatim region (@keyframes, statement-form "@layer x, y;"),
      // and left body font_faces() untouched; a font-face-only body
      // therefore keeps its group. Depth is bounded by the parser's
      // group-nesting cap.
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
      // CHECK on type). Everything the parser kept verbatim stays: these are
      // at-rules no browser report can judge --- @keyframes (a rule that
      // names an animation is of no use without them), @property,
      // @counter-style, @font-feature-values, @page, statement-form
      // "@layer a, b;" (whose layer declarations are load-bearing for
      // cascade order), and anything the parser did not understand.
      continue;
    }

    // TODO(morlovich): This does a lot of repeated work as the same media
    // entries are repeated for tons of rulesets.
    // TODO(morlovich): It's silly to serialize this, we should work directly
    // off AST once we have decision procedure on that.

    bool any_media_apply =
        FilterMediaQueriesForScreen(&r->mutable_media_queries());

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
        // Keep a selector that is critical, and one no browser has been
        // asked about yet: after a stylesheet change its rule must not be
        // dropped on the strength of reports that predate it. A selector is
        // left out only while reports show it unmatched.
        if (portion_to_compare.empty() ||
            critical_selectors_.find(portion_to_compare) !=
                critical_selectors_.end() ||
            (keep_unknown_selectors_ &&
             known_selectors_.find(portion_to_compare) ==
                 known_selectors_.end())) {
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
  const SummaryInfo& summary = GetSummaryForStyle(pos);
  DCHECK_EQ(kSummaryOk, summary.state);

  // Only external stylesheets are deferred. An inline <style> block is
  // already in the page, so there is nothing to fetch later; it stays where
  // it is, complete, and so keeps its place in the cascade between the
  // stylesheets around it.
  if (char_node != nullptr || !summary.is_external) {
    return;
  }

  // A stylesheet that must stay as it is: it imports another one, or nearly
  // all of it would be inline (see Summarize).
  if (summary.data == kKeepBlockingSummary) {
    return;
  }

  // A stylesheet the page keeps for browsers without scripts, and an
  // alternate stylesheet, do not block rendering; leave them alone.
  if (summary.is_inside_noscript ||
      CssTagScanner::IsAlternateStylesheet(summary.rel)) {
    return;
  }

  // A stylesheet the page has switched off must not have its rules applied
  // from an inline block, and a link with a title belongs to a named set of
  // stylesheets that the browser, the visitor or a script can switch between;
  // leave both alone.
  if (element->FindAttribute(HtmlName::kDisabled) != nullptr ||
      element->FindAttribute(HtmlName::kTitle) != nullptr) {
    return;
  }

  // The preload below is a copy of the link. An event handler on it would
  // run when the preload arrives and again when the stylesheet applies, the
  // first time before the styles are there; leave such a link alone.
  const HtmlElement::AttributeList& own_attrs = element->attributes();
  for (HtmlElement::AttributeConstIterator i(own_attrs.begin());
       i != own_attrs.end(); ++i) {
    const HtmlElement::Attribute& attr = *i;
    if (StringCaseStartsWith(attr.name_str(), "on")) {
      return;
    }
  }

  // Likewise a stylesheet none of whose media can apply to a screen.
  StringVector all_media;
  css_util::VectorizeMediaAttribute(summary.media_from_html, &all_media);
  StringVector relevant_media;
  for (int i = 0, n = all_media.size(); i < n; ++i) {
    const GoogleString& medium = all_media[i];
    if (css_util::CanMediaAffectScreen(medium)) {
      relevant_media.push_back(medium);
    }
  }
  if (!all_media.empty() && relevant_media.empty()) {
    return;
  }

  // The deferred stylesheet is turned back on by an inline <script>; a
  // script-src policy without 'unsafe-inline' would block it. The
  // authoritative gate is PolicyPermitsRendering(), so with a forbidding
  // policy we don't get here at all; this is a defensive backstop.
  if (!CspPermitsInlineScript()) {
    return;
  }

  // If we're inlining an external CSS file, make sure to adjust the URLs
  // inside to the new base.
  const GoogleString* css_to_use = &summary.data;
  GoogleString resolved_css;
  StringWriter writer(&resolved_css);
  GoogleUrl input_css_base(summary.base);
  if (driver()->ResolveCssUrls(
          input_css_base, driver()->base_url().Spec(), summary.data, &writer,
          driver()->message_handler()) == RewriteDriver::kSuccess) {
    css_to_use = &resolved_css;
  }

  // Inlining bytes whose charset differs from the page's can garble them
  // (e.g. non-UTF-8 content: strings) --- the equivalent of
  // CssInlineFilter::ShouldInline()'s charset check. A pure-ASCII critical
  // subset is charset-agnostic, so it is still safe. Otherwise leave this
  // stylesheet alone: its link stays in place and blocking.
  //
  // An empty summary.charset means the stylesheet declared no charset (no
  // Content-Type charset, @charset, BOM, or charset attribute). We cannot
  // assume it matches the page, so treat unknown as "differs" and apply the
  // same non-ASCII bail-out --- otherwise mismatched bytes would be inlined
  // and garbled. CssInlineFilter avoids this by defaulting the charset before
  // comparing; here we bail whenever the charset is unknown or differs.
  if (summary.charset.empty() ||
      !StringCaseEqual(driver()->containing_charset(), summary.charset)) {
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

  // Security: bytes that contain a closing style tag would end the inline
  // <style> element early and turn the rest into markup. The summarizer
  // already refuses a summary that contains one; this checks the bytes that
  // are actually written, after URL resolution, whatever produced them.
  // Mirror CssInlineFilter::HasClosingStyleTag: if the critical subset
  // contains a closing style tag, leave this stylesheet alone.
  if (FindIgnoreCase(*css_to_use, "</style") != StringPiece::npos) {
    return;
  }

  HtmlElement::Attribute* rel = element->FindAttribute(HtmlName::kRel);
  if (rel == nullptr) {
    return;
  }

  // The critical rules, inline, where the link stood. No empty block.
  if (!css_to_use->empty()) {
    HtmlElement* style_element =
        driver()->NewElement(nullptr, HtmlName::kStyle);
    // Carry over attributes the page may depend on: id (stylesheet toggling
    // by script) and data-*. media is reconstructed below; the link-specific
    // attributes (href, rel, type, charset) don't apply to a style element.
    const HtmlElement::AttributeList& link_attrs = element->attributes();
    for (HtmlElement::AttributeConstIterator i(link_attrs.begin());
         i != link_attrs.end(); ++i) {
      const HtmlElement::Attribute& attr = *i;
      if (attr.keyword() == HtmlName::kId ||
          StringCaseStartsWith(attr.name_str(), "data-")) {
        style_element->AddAttribute(attr);
      }
    }
    // Just the media that is relevant to screen.
    if (!relevant_media.empty()) {
      driver()->AddAttribute(style_element, HtmlName::kMedia,
                             css_util::StringifyMediaVector(relevant_media));
    }
    driver()->InsertNodeBeforeNode(element, style_element);
    HtmlCharactersNode* content =
        driver()->NewCharactersNode(style_element, *css_to_use);
    driver()->AppendChild(style_element, content);
  }

  // The script that turns deferred stylesheets on, once per document and
  // ahead of the first of them, so it is listening before any can arrive.
  if (!loader_inserted_) {
    loader_inserted_ = true;
    HtmlElement* script = driver()->NewElement(nullptr, HtmlName::kScript);
    driver()->AddAttribute(script, HtmlName::kDataPagespeedNoDefer,
                           StringPiece());
    driver()->InsertNodeBeforeNode(element, script);
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

  // The full stylesheet, as optimized by the filters that ran before us: a
  // preload in the link's own place, which the loader makes the stylesheet
  // again there. It carries the link's attributes so the browser reuses the
  // preloaded file. The id goes with the inline block when there is one.
  HtmlElement* preload = driver()->CloneElement(element);
  if (!css_to_use->empty()) {
    preload->DeleteAttribute(HtmlName::kId);
  }
  preload->DeleteAttribute(HtmlName::kAs);
  HtmlElement::Attribute* preload_rel = preload->FindAttribute(HtmlName::kRel);
  if (preload_rel != nullptr) {
    preload_rel->SetValue("preload");
  }
  driver()->AddAttribute(preload, HtmlName::kAs, "style");
  driver()->AddAttribute(preload, kDeferredCssAttribute, StringPiece());
  driver()->InsertNodeBeforeNode(element, preload);

  // And the link as it was, for browsers that run no scripts.
  HtmlElement* noscript = driver()->NewElement(nullptr, HtmlName::kNoscript);
  driver()->InsertNodeBeforeNode(element, noscript);
  driver()->AppendChild(noscript, driver()->CloneElement(element));

  *is_element_deleted = driver()->DeleteNode(element);
}

GoogleString CriticalSelectorFilter::CacheKeySuffix() const {
  return cache_key_suffix_;
}

void CriticalSelectorFilter::StartDocumentImpl() {
  CssSummarizerBase::StartDocumentImpl();
  ServerContext* context = driver()->server_context();

  // Read critical selector info from pcache.
  CriticalSelectorFinder* finder = context->critical_selector_finder();
  critical_selectors_ = finder->GetCriticalSelectors(driver());
  known_selectors_ = finder->GetKnownSelectors(driver());
  keep_unknown_selectors_ = finder->TracksCandidateSelectors();

  // Compute corresponding cache key suffix: the inline subset depends on
  // which selectors are critical and on which have been judged at all.
  GoogleString all_selectors =
      StrCat(JoinCollection(critical_selectors_, ","), "\n",
             JoinCollection(known_selectors_, ","), "\n",
             keep_unknown_selectors_ ? "1" : "0");
  cache_key_suffix_ = context->lock_hasher()->Hash(all_selectors);

  // Clear state between re-uses.
  loader_inserted_ = false;
}

void CriticalSelectorFilter::DetermineEnabled(GoogleString* disabled_reason) {
  // We shouldn't do anything if there is no information on critical selectors
  // in the property cache, or if that information does not describe the page
  // as it is now (its selectors grew, or the last report was cut short) and
  // no complete report on the current set has arrived yet: the stylesheets
  // then stay blocking, which is always correct. Unfortunately, we also
  // cannot run safely in case of IE, since we do not understand IE
  // conditional comments well enough to replicate their behavior.
  CriticalSelectorFinder* finder =
      driver()->server_context()->critical_selector_finder();
  const StringSet& critical_selectors = finder->GetCriticalSelectors(driver());
  bool has_current_data = finder->HasCurrentBeaconData(driver());
  bool ua_supports_critical_css =
      driver()->request_properties()->SupportsCriticalCss();
  bool can_run = ua_supports_critical_css && !critical_selectors.empty() &&
                 has_current_data;
  driver()->log_record()->LogRewriterHtmlStatus(
      RewriteOptions::FilterId(RewriteOptions::kPrioritizeCriticalCss),
      (can_run ? RewriterHtmlApplication::ACTIVE
               : (ua_supports_critical_css
                      ? RewriterHtmlApplication::PROPERTY_CACHE_MISS
                      : RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED)));

  if (!can_run) {
    if (!ua_supports_critical_css) {
      *disabled_reason = "User agent not supported";
    } else if (critical_selectors.empty()) {
      *disabled_reason = "No critical selector info in cache";
    } else {
      *disabled_reason =
          "Waiting for a browser to report on the page's current stylesheets";
    }
  }

  set_is_enabled(can_run);
}

}  // namespace net_instaweb

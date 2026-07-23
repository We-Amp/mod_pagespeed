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

#include "net/instaweb/rewriter/public/collect_dependencies_filter.h"

#include <memory>

#include "base/logging.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/dependencies.pb.h"
#include "net/instaweb/rewriter/input_info.pb.h"
#include "net/instaweb/rewriter/public/css_util.h"
#include "net/instaweb/rewriter/public/dependency_tracker.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_result.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/http/data_url.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "third_party/css_parser/src/util/utf8/public/unicodetext.h"
#include "third_party/css_parser/src/webutil/css/parser.h"
#include "third_party/css_parser/src/webutil/css/value.h"

namespace net_instaweb {

namespace {

// Fonts are only worth preloading when they plausibly affect first render of
// ordinary text. A @font-face whose unicode-range does not intersect
// printable ASCII (U+0020 - U+007E) is a specialized subset (symbols-only,
// non-latin-only extension slices, etc.) that the first view likely never
// renders, so we skip it rather than risk preloading unused bytes.
const uint32 kBasicCoverageBegin = 0x20;
const uint32 kBasicCoverageEnd = 0x7E;

// Parses one unicode-range token --- U+XXXX, U+XXXX-YYYY, U+XX??, or U+??
// --- into an inclusive [begin, end] codepoint range. Returns false for
// anything else.
bool ParseUnicodeRangeToken(StringPiece token, uint32* begin_out,
                            uint32* end_out) {
  TrimWhitespace(&token);
  int n = token.size();
  int pos = 0;
  if (n < 3 || (token[0] != 'U' && token[0] != 'u') || token[1] != '+') {
    return false;
  }
  pos = 2;

  uint32 begin = 0;
  int num_digits = 0;
  while (pos < n && num_digits < 6 && AccumulateHexValue(token[pos], &begin)) {
    ++pos;
    ++num_digits;
  }

  // Wildcard form: remaining chars must all be '?', each widening the range
  // by one hex digit (U+30?? means U+3000 - U+30FF). Valid without leading
  // hex digits too: U+?? means U+00 - U+FF.
  if (pos < n && token[pos] == '?') {
    uint32 end = begin;
    while (pos < n && token[pos] == '?' && num_digits < 6) {
      begin *= 16;
      end = end * 16 + 0xF;
      ++pos;
      ++num_digits;
    }
    if (pos != n) {
      return false;
    }
    *begin_out = begin;
    *end_out = end;
    return true;
  }

  if (num_digits == 0) {
    return false;
  }

  if (pos == n) {  // Single codepoint.
    *begin_out = begin;
    *end_out = begin;
    return true;
  }

  if (token[pos] != '-') {
    return false;
  }
  ++pos;

  uint32 end = 0;
  num_digits = 0;
  while (pos < n && num_digits < 6 && AccumulateHexValue(token[pos], &end)) {
    ++pos;
    ++num_digits;
  }
  if (num_digits == 0 || pos != n || end < begin) {
    return false;
  }
  *begin_out = begin;
  *end_out = end;
  return true;
}

// True if any well-formed token in a unicode-range value (comma-separated
// list) intersects printable ASCII. Tokens we cannot parse contribute no
// coverage, so a fully malformed value reads as "no basic coverage" ---
// erring towards not preloading.
bool UnicodeRangeIntersectsBasicText(StringPiece value_text) {
  StringPieceVector tokens;
  SplitStringPieceToVector(value_text, ",", &tokens, true /* omit_empty */);
  for (StringPiece token : tokens) {
    uint32 begin, end;
    if (ParseUnicodeRangeToken(token, &begin, &end) &&
        begin <= kBasicCoverageEnd && end >= kBasicCoverageBegin) {
      return true;
    }
  }
  return false;
}

// Reconstructs the value text of a cleanly-parsed (Property::OTHER)
// unicode-range declaration from its parsed values, returning false when the
// values take any other shape. Digit-leading, wildcard-free tokens lex as an
// IDENT "U" followed by NUMBER values whose "-XXXX" tail became a dimension
// unit (see FontFaceCoversBasicText); preservation mode stores each number's
// verbatim bytes, so IDENT text + verbatim number bytes + dimension unit
// text reproduces the original token exactly, with COMMA values as ","
// separators. Anything else (strings, functions, ...) cannot be
// reconstructed faithfully, and a NUMBER without verbatim bytes (parsing
// without preservation mode) has unrecoverable text.
bool ReconstructUnicodeRangeValue(const Css::Declaration& decl,
                                  GoogleString* text_out) {
  if (decl.values() == nullptr) {
    return false;
  }
  GoogleString text;
  const Css::Values& values = *decl.values();
  for (int i = 0, n = values.size(); i < n; ++i) {
    const Css::Value* value = values.get(i);
    switch (value->GetLexicalUnitType()) {
      case Css::Value::IDENT:
        text += UnicodeTextToUTF8(value->GetIdentifierText());
        break;
      case Css::Value::NUMBER: {
        CssStringPiece bytes = value->bytes_in_original_buffer();
        if (bytes.empty()) {
          return false;
        }
        text.append(bytes.data(), bytes.size());
        text += value->GetDimensionUnitText();
        break;
      }
      case Css::Value::COMMA:
        text += ",";
        break;
      default:
        return false;
    }
  }
  *text_out = text;
  return true;
}

// Decides whether a face's unicode-range (if any) plausibly covers ordinary
// text. The CSS parser has no notion of unicode-range, and its declarations
// reach us in one of two shapes:
//  - Letter-leading or wildcard forms (U+FEFF, U+26??) fail value parsing;
//    in preservation mode the whole declaration is demoted to a
//    Property::UNPARSEABLE dummy carrying verbatim bytes (without setting
//    the parser error mask). We scan those bytes for ASCII intersection.
//  - Digit-leading, wildcard-free forms (U+0400-045F) lex CLEANLY --- as
//    IDENT "U" plus a NUMBER whose "-045F" tail becomes a unit (ParseNumber
//    accepts '-' as a unit start via StartsIdent) --- so the declaration
//    comes out as ordinary Property::OTHER. The original token text is then
//    rebuilt from the parsed values (see ReconstructUnicodeRangeValue) and
//    evaluated the same way; only values we cannot reconstruct stay
//    unevaluatable and read as "no basic coverage".
// Per CSS cascade the last unicode-range declaration in the face wins; a
// face without any unicode-range declaration is assumed to cover basic
// text.
bool FontFaceCoversBasicText(const Css::FontFace& face) {
  bool has_range = false;
  bool winner_scannable = false;
  GoogleString range_text;
  for (const Css::Declaration* decl : face.declarations()) {
    if (decl->property().prop() == Css::Property::OTHER) {
      if (StringCaseEqual(decl->prop_text(), "unicode-range")) {
        // Parsed cleanly: rebuild the token text from the parsed values
        // (digit-leading ranges lex as IDENT "U" + NUMBERs) and evaluate it.
        has_range = true;
        winner_scannable = ReconstructUnicodeRangeValue(*decl, &range_text);
      }
      continue;
    }
    if (decl->property().prop() != Css::Property::UNPARSEABLE) {
      continue;
    }
    CssStringPiece raw = decl->bytes_in_original_buffer();
    StringPiece bytes(raw.data(), raw.size());
    stringpiece_ssize_type colon = bytes.find(':');
    if (colon == StringPiece::npos) {
      continue;
    }
    StringPiece name = bytes.substr(0, colon);
    TrimWhitespace(&name);
    if (!StringCaseEqual(name, "unicode-range")) {
      continue;
    }
    has_range = true;
    winner_scannable = true;
    range_text = bytes.substr(colon + 1).as_string();
  }
  if (!has_range) {
    return true;
  }
  if (!winner_scannable) {
    return false;
  }
  return UnicodeRangeIntersectsBasicText(range_text);
}

// Finds the first woff2 candidate in the face's src declaration (per CSS
// cascade, the last src declaration in the face). A candidate is a url()
// value immediately followed by format("woff2"), or a bare url() --- no
// format() hint --- whose path, resolved against base_url, ends in .woff2.
// A url() explicitly declared as another format is never a candidate, and a
// face without any woff2 candidate is skipped entirely: guessing among
// fallback sources is user-agent-dependent, and a wrong guess means a double
// fetch. Returns true and sets *url_out to the URL as written in the CSS.
bool FindWoff2SrcUrl(const Css::FontFace& face, const GoogleUrl& base_url,
                     GoogleString* url_out) {
  const Css::Declaration* src_decl = nullptr;
  for (const Css::Declaration* decl : face.declarations()) {
    // src is not in the parser's property table, so it comes out as OTHER
    // with the property text preserved.
    if (decl->property().prop() == Css::Property::OTHER &&
        StringCaseEqual(decl->prop_text(), "src")) {
      src_decl = decl;  // Last src declaration wins.
    }
  }
  if (src_decl == nullptr || src_decl->values() == nullptr) {
    return false;
  }

  const Css::Values& values = *src_decl->values();
  for (int i = 0, n = values.size(); i < n; ++i) {
    const Css::Value* value = values.get(i);
    if (value->GetLexicalUnitType() != Css::Value::URI) {
      continue;
    }
    GoogleString url = UnicodeTextToUTF8(value->GetStringValue());

    const Css::Value* next = (i + 1 < n) ? values.get(i + 1) : nullptr;
    if (next != nullptr && next->GetLexicalUnitType() == Css::Value::FUNCTION &&
        StringCaseEqual(UnicodeTextToUTF8(next->GetFunctionName()), "format")) {
      // Explicit format hint: trust it, in either direction.
      const Css::Values* params = next->GetParameters();
      if (params != nullptr && params->size() >= 1) {
        const Css::Value* param = params->get(0);
        GoogleString format;
        if (param->GetLexicalUnitType() == Css::Value::STRING) {
          format = UnicodeTextToUTF8(param->GetStringValue());
        } else if (param->GetLexicalUnitType() == Css::Value::IDENT) {
          format = UnicodeTextToUTF8(param->GetIdentifierText());
        }
        if (StringCaseEqual(format, "woff2")) {
          *url_out = url;
          return true;
        }
      }
      continue;
    }

    // No format() hint: accept when the URL path itself says woff2.
    GoogleUrl resolved(base_url, url);
    if (resolved.IsWebValid() &&
        StringCaseEndsWith(resolved.PathSansQuery(), ".woff2")) {
      *url_out = url;
      return true;
    }
  }
  return false;
}

}  // namespace

class CollectDependenciesFilter::Context : public RewriteContext {
 public:
  Context(DependencyType type, RewriteDriver* driver)
      : RewriteContext(driver, nullptr, nullptr),
        mutex_(driver->server_context()->thread_system()->NewMutex()),
        reported_(false),
        dep_type_(type),
        dep_id_(-1) {}

  void Initiated() {
    dep_id_ = Driver()->dependency_tracker()->RegisterDependencyCandidate();
  }

  ~Context() override { CHECK(reported_ || dep_id_ == -1); }

  bool Partition(OutputPartitions* partitions,
                 OutputResourceVector* outputs) override {
    // We will never produce output, but always want to do stuff.
    outputs->push_back(OutputResourcePtr(nullptr));
    partitions->add_partition();

    ResourcePtr resource(slot(0)->resource());
    if (resource->loaded()) {
      resource->AddInputInfoToPartition(Resource::kIncludeInputHash, 0,
                                        partitions->mutable_partition(0));
    }
    return true;
  }

  static bool DefinitelyNeededToRender(
      const std::unique_ptr<Css::Import>& import) {
    StringVector media_types;
    if (!css_util::ConvertMediaQueriesToStringVector(import->media_queries(),
                                                     &media_types)) {
      // Something we don't understand. This includes things specifying
      // media queries, which we can't evaluate, and therefore conservatively
      // assume to be potentially unneeded.
      return false;
    }
    return DefinitelyNeededToRender(media_types);
  }

  static bool DefinitelyNeededToRender(const StringVector& media_types) {
    if (media_types.empty()) {
      return true;  // @import "foo", without media specified.
    }

    for (auto& medium : media_types) {
      if (StringCaseEqual(medium, "all") || StringCaseEqual(medium, "screen")) {
        return true;
      }
    }
    return false;
  }

 protected:
  void ExtractNestedCssDependencies(const Dependency* parent_dep,
                                    const ResourcePtr& resource,
                                    CachedResult* partition) {
    // TODO(morlovich): We should probably look inside <style> blocks like this,
    // too?

    // Don't crash out on resources without anything loaded, and don't try to
    // parse error pages for CSS imports.
    if (!resource->HttpStatusOk()) {
      return;
    }
    // Parse over the full byte range. ExtractUncompressedContents() returns a
    // StringPiece with no NUL-termination guarantee, so the single-arg
    // Css::Parser(const char*) ctor (which does strlen()) would over-read past
    // the buffer, and any embedded '\0' in the CSS would truncate parsing and
    // silently drop later @imports. Bound parsing by size() instead.
    StringPiece css = resource->ExtractUncompressedContents();
    Css::Parser parser(css.data(), css.data() + css.size());
    parser.set_preservation_mode(true);
    // We avoid quirks-mode so that we do not "fix" something we shouldn't have.
    parser.set_quirks_mode(false);

    while (true) {
      std::unique_ptr<Css::Import> import(parser.ParseNextImport());
      if (import == nullptr ||
          parser.errors_seen_mask() != Css::Parser::kNoError) {
        break;
      }

      if (DefinitelyNeededToRender(import)) {
        GoogleString rel_url(import->link().utf8_data(),
                             import->link().utf8_length());
        GoogleUrl full_url(GoogleUrl(resource->url()), rel_url);
        if (full_url.IsWebValid()) {
          Dependency* dep = partition->add_collected_dependency();
          dep->set_url(full_url.Spec().as_string());
          dep->set_content_type(DEP_CSS);
          *dep->mutable_validity_info() = parent_dep->validity_info();
        }
      }
    }
  }

  // Collects woff2 fonts declared by @font-face rules in the CSS, into the
  // dedicated collected_font_dependency field --- never into
  // collected_dependency, so binaries that predate DEP_FONT skip them as
  // unknown fields (see dependencies.proto). Unlike the @import preamble
  // scan above, this needs a full stylesheet parse; it runs only on metadata
  // cache misses (Report() replays cached results).
  void ExtractFontDependencies(const Dependency* parent_dep,
                               const ResourcePtr& resource,
                               CachedResult* partition) {
    if (!resource->HttpStatusOk()) {
      return;
    }
    // Parse over the full byte range, as in ExtractNestedCssDependencies
    // above: the strlen-based ctor would read whatever follows the
    // un-terminated body in the backing buffer (e.g. serialized headers),
    // poisoning the error mask and vetoing the harvest below.
    StringPiece css = resource->ExtractUncompressedContents();
    Css::Parser parser(css.data(), css.data() + css.size());
    parser.set_preservation_mode(true);
    // We avoid quirks-mode so that we do not "fix" something we shouldn't
    // have.
    parser.set_quirks_mode(false);
    std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseStylesheet());
    if (stylesheet == nullptr ||
        parser.errors_seen_mask() != Css::Parser::kNoError) {
      // Don't harvest fonts from CSS we could not fully make sense of.
      // Note that in preservation mode, declarations whose values fail to
      // lex (such as letter-leading or wildcard unicode-range forms) are
      // demoted to unparseable dummies and do not set the error mask;
      // digit-leading unicode-range forms lex cleanly and never error (see
      // FontFaceCoversBasicText).
      return;
    }
    GoogleUrl base_url(resource->url());
    for (const Css::FontFace* face : stylesheet->font_faces()) {
      // Same conservative media rule as for imports: anything we can't
      // evaluate is treated as potentially unneeded.
      StringVector media_types;
      if (!css_util::ConvertMediaQueriesToStringVector(face->media_queries(),
                                                       &media_types) ||
          !DefinitelyNeededToRender(media_types)) {
        continue;
      }
      if (!FontFaceCoversBasicText(*face)) {
        continue;
      }
      GoogleString rel_url;
      if (!FindWoff2SrcUrl(*face, base_url, &rel_url)) {
        continue;
      }
      GoogleUrl full_url(base_url, rel_url);
      if (full_url.IsWebValid()) {
        Dependency* dep = partition->add_collected_font_dependency();
        dep->set_url(full_url.Spec().as_string());
        dep->set_content_type(DEP_FONT);
        *dep->mutable_validity_info() = parent_dep->validity_info();
      }
    }
  }

  void Rewrite(int partition_index, CachedResult* partition,
               const OutputResourcePtr& output_resource) override {
    Dependency* dep = partition->add_collected_dependency();
    dep->set_url(slot(0)->resource()->url());
    dep->set_content_type(dep_type_);

    // The framework collected input info from any filter that ran before
    // us, but not us (since it will do it after we finish work) --- which
    // matters if our input is an unoptimized result, so add in our input info.
    for (int i = 0; i < partition->input_size(); ++i) {
      slot(0)->ReportInput(partition->input(i));
    }

    if (slot(0)->inputs() != nullptr) {
      for (const InputInfo& input : *slot(0)->inputs()) {
        InputInfo* stored_copy = dep->add_validity_info();
        *stored_copy = input;

        // Drop the parts of the info we can't use for checking validity
        // of push.
        stored_copy->clear_input_content_hash();
        stored_copy->clear_disable_further_processing();
        stored_copy->clear_index();
      }
    }

    // Note: this needs to happen after the above since we need to propagate
    // validity_info.
    if (dep_type_ == DEP_CSS) {
      ExtractNestedCssDependencies(dep, slot(0)->resource(), partition);
      ExtractFontDependencies(dep, slot(0)->resource(), partition);
    }

    // TODO(morlovich): is_pagespeed_resource is not currently set, but I am not
    // sure I actually want that: validity_info may be useful for non-optimized
    // resources as well, and we set that already.

    CHECK(output_resource.get() == nullptr);
    CHECK_EQ(0, partition_index);
    RewriteDone(kRewriteFailed, 0);
  }

  OutputResourceKind kind() const override { return kOnTheFlyResource; }

  const char* id() const override { return "cdf"; }

  bool PolicyPermitsRendering() const override {
    return true;  // We don't alter the doc...
  }

  void Render() override { Report(); }

  void WillNotRender() override {
    {
      ScopedMutex hold(mutex_.get());
      if (reported_) {
        return;
      }
      reported_ = true;
    }

    // We don't have results in time (and if we did, we wouldn't be able to
    // access them from this thread), so give up on propagating to pcache for
    // this time. This is somewhat conservative: if this is actually an early
    // flush window we could deliver the result to depedency_tracker safely,
    // but then if it's after document end it would have us miss the cache
    // commit entirely...
    Driver()->dependency_tracker()->ReportDependencyCandidate(dep_id_, nullptr);
  }

  void Cancel() override { Report(); }

 private:
  void Report() {
    {
      ScopedMutex hold(mutex_.get());
      if (reported_) {
        return;
      }
      reported_ = true;
    }

    DependencyTracker* dep_tracker = Driver()->dependency_tracker();

    // We already allocated dep_id_, so we should report on it, with either
    // the first dependency we collected, or nullptr.
    if (num_output_partitions() == 1 &&
        output_partition(0)->collected_dependency_size() > 0) {
      // Deep copy here because output_partition is already written, and it
      // makes no sense to mutate it.
      CachedResult result = *output_partition(0);

      // Top-level stuff just gets its dep_id_ as the sorting key.
      result.mutable_collected_dependency(0)->add_order_key(dep_id_);

      dep_tracker->ReportDependencyCandidate(dep_id_,
                                             &result.collected_dependency(0));

      // Any other dependencies stored in result->collected_dependency >= 1
      // are things we discovered *inside* whatever is described by
      // result->collected_dependency(0)
      //
      // We grab a brand new ID for each one's storage inside
      // dependency_tracker, and give them sorting keys based on the parent's
      // dep_id_: (dep_id_, 1), (dep_id_, 2), etc., and so on, to make them get
      // sorted after their parent (whose sorting key will be (dep_id_)) and
      // before the next top-level resource, which will be something like
      // (dep_id_ + 1) or some larger number. Note that we produce order keys
      // at most 2 deep because we (for now?) only collect dependencies that
      // deep.
      for (int c = 1; c < result.collected_dependency_size(); ++c) {
        int additional_dep_id = dep_tracker->RegisterDependencyCandidate();
        Dependency* child_dep = result.mutable_collected_dependency(c);
        child_dep->add_order_key(dep_id_);
        child_dep->add_order_key(c);
        dep_tracker->ReportDependencyCandidate(additional_dep_id, child_dep);
      }

      // Fonts discovered inside collected_dependency(0) are children in the
      // same sense as the entries above, so they continue the same child
      // index sequence --- one single (dep_id_, c) key space for imports and
      // fonts --- keeping order keys lexicographically consistent.
      int next_child_index = result.collected_dependency_size();
      for (int f = 0; f < result.collected_font_dependency_size(); ++f) {
        int additional_dep_id = dep_tracker->RegisterDependencyCandidate();
        Dependency* font_dep = result.mutable_collected_font_dependency(f);
        font_dep->add_order_key(dep_id_);
        font_dep->add_order_key(next_child_index + f);
        dep_tracker->ReportDependencyCandidate(additional_dep_id, font_dep);
      }
    } else {
      dep_tracker->ReportDependencyCandidate(dep_id_, nullptr);
    }
  }

  std::unique_ptr<AbstractMutex> mutex_;
  bool reported_ GUARDED_BY(mutex_);
  DependencyType dep_type_;
  int dep_id_;

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
};

CollectDependenciesFilter::CollectDependenciesFilter(RewriteDriver* driver)
    : CommonFilter(driver) {}

void CollectDependenciesFilter::StartDocumentImpl() {}

void CollectDependenciesFilter::StartElementImpl(HtmlElement* element) {
  // We generally don't want noscript path stuff, since it's not usually
  // used.
  if (noscript_element() != nullptr) {
    // Do nothing
    return;
  }

  resource_tag_scanner::UrlCategoryVector attributes;
  resource_tag_scanner::ScanElement(element, driver()->options(), &attributes);
  for (int i = 0, n = attributes.size(); i < n; ++i) {
    // We only collect scripts and CSS.
    if (attributes[i].category == semantic_type::kStylesheet ||
        attributes[i].category == semantic_type::kScript) {
      HtmlElement::Attribute* attr = attributes[i].url;
      StringPiece url(attr->DecodedValueOrNull());
      if (url.empty() || IsDataUrl(url)) {
        continue;
      }

      // Module scripts need rel=modulepreload; a plain as=script preload
      // occupies a different preload-cache slot than the module map fetch,
      // so hinting one only causes a double fetch. Skip them until
      // modulepreload emission exists.
      if (attributes[i].category == semantic_type::kScript) {
        const HtmlElement::Attribute* type =
            element->FindAttribute(HtmlName::kType);
        if (type != nullptr && type->DecodedValueOrNull() != nullptr &&
            ScriptTagScanner::Normalized(type->DecodedValueOrNull()) ==
                "module") {
          continue;
        }
      }

      // Check media on standard stylesheets.
      if (attributes[i].category == semantic_type::kStylesheet &&
          element->keyword() == HtmlName::kLink &&
          attr->keyword() == HtmlName::kHref) {
        HtmlElement::Attribute* media =
            element->FindAttribute(HtmlName::kMedia);
        if (media != nullptr) {
          if (media->DecodedValueOrNull() == nullptr) {
            // Encoding weirdness with media attribute -> don't push
            // skip CSS with null media
            continue;
          }
          StringVector media_vector;
          css_util::VectorizeMediaAttribute(media->DecodedValueOrNull(),
                                            &media_vector);
          if (!Context::DefinitelyNeededToRender(media_vector)) {
            // skip CSS not needed to render
            continue;
          }
        }
      }

      // Code below relies on this being the guard here for it to be safe.
      CHECK(attributes[i].category == semantic_type::kStylesheet ||
            attributes[i].category == semantic_type::kScript);
      RewriteDriver::InputRole role =
          (attributes[i].category == semantic_type::kStylesheet
               ? RewriteDriver::InputRole::kStyle
               : RewriteDriver::InputRole::kScript);
      ResourcePtr resource(
          CreateInputResourceOrInsertDebugComment(url, role, element));
      if (resource.get() == nullptr) {
        // TODO(morlovich): This may mean a valid 3rd party resource;
        // we also probably don't want a warning in that case.
        continue;
      }
      ResourceSlotPtr slot(driver()->GetSlot(resource, element, attr));
      slot->set_need_aggregate_input_info(true);
      Context* context = new Context(
          attributes[i].category == semantic_type::kStylesheet ? DEP_CSS
                                                               : DEP_JAVASCRIPT,
          driver());
      context->AddSlot(slot);
      if (driver()->InitiateRewrite(context)) {
        context->Initiated();
      }
    }
  }
}

void CollectDependenciesFilter::EndElementImpl(HtmlElement* element) {}

}  // namespace net_instaweb

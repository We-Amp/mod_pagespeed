// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Shared machinery for the design record D2 differential HTML parse harness
// (tools/html-parse-corpus/SPEC.md): a recording HtmlFilter that renders the
// parse event stream, plus the parse-session driver. Header-only so the
// probe (html_parse_probe.cc) and the fuzz target (html_fuzz.cc) exercise
// exactly the same code path without adding sources to the kernel library.
//
// The stream format, session setup, and exit codes are contractual; see
// SPEC.md v1. ModPageSpeed 2.0 re-implements this sink against lib/html and
// must produce byte-identical output for the same inputs.

#ifndef PAGESPEED_KERNEL_HTML_HTML_PARSE_RECORDER_H_
#define PAGESPEED_KERNEL_HTML_HTML_PARSE_RECORDER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/doctype.h"
#include "pagespeed/kernel/html/empty_html_filter.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/http/content_type.h"

namespace net_instaweb {
namespace html_parse_probe {

// SPEC.md SS2: fixed parse-session parameters. The URL must stay parseable
// by any conforming GoogleUrl equivalent; it is never emitted.
inline const char* FixedBaseUrl() {
  return "http://html-parse-corpus.invalid/";
}

// SPEC.md SS4 exit codes.
inline constexpr int kExitOk = 0;
inline constexpr int kExitHarnessError = 2;
inline constexpr int kExitSizeLimit = 3;

// SPEC.md SS3.2: FNV-1a, 64-bit.
inline uint64_t Fnv1a64(const char* data, size_t size) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < size; ++i) {
    h ^= static_cast<unsigned char>(data[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

// SPEC.md SS3.1: escape a field so it contains no byte outside [0x21,0x7E]
// and no backslash ambiguity: '\\' -> "\\\\", others outside the range ->
// "\xHH" (lowercase hex).
inline void AppendEscaped(const StringPiece& in, GoogleString* out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < in.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(in[i]);
    if (b == '\\') {
      out->append("\\\\", 2);
    } else if (b < 0x21 || b > 0x7e) {
      char esc[4] = {'\\', 'x', kHex[b >> 4], kHex[b & 0xf]};
      out->append(esc, sizeof(esc));
    } else {
      *out += static_cast<char>(b);
    }
  }
}

// Canonical lowercase keyword string, or "-" for kNotAKeyword.
inline void AppendKeyword(HtmlName::Keyword keyword, GoogleString* out) {
  const StringPiece* str = HtmlKeywords::KeywordToString(keyword);
  if (str == nullptr) {
    *out += '-';
  } else {
    // Use append() rather than StringPiece::AppendToString, which
    // std::string_view-backed StringPiece dialects do not provide.
    out->append(str->data(), str->size());
  }
}

// Source spelling of a name, escaped.
inline void AppendSpelling(const HtmlName& name, GoogleString* out) {
  AppendEscaped(name.value(), out);
}

inline void AppendLeaf(const char* kind, const HtmlLeafNode* node,
                       GoogleString* out) {
  const GoogleString& contents = node->contents();
  char buf[96];
  snprintf(buf, sizeof(buf), "%s len=%zu fnv=%016llx\n", kind, contents.size(),
           static_cast<unsigned long long>(
               Fnv1a64(contents.data(), contents.size())));
  *out += buf;
}

inline char QuoteChar(HtmlElement::QuoteStyle q) {
  switch (q) {
    case HtmlElement::NO_QUOTE:
      return 'n';
    case HtmlElement::SINGLE_QUOTE:
      return 's';
    case HtmlElement::DOUBLE_QUOTE:
      return 'd';
  }
  return '?';
}

inline const char* StyleName(HtmlElement::Style style) {
  switch (style) {
    case HtmlElement::AUTO_CLOSE:
      return "auto";
    case HtmlElement::IMPLICIT_CLOSE:
      return "implicit";
    case HtmlElement::EXPLICIT_CLOSE:
      return "explicit";
    case HtmlElement::BRIEF_CLOSE:
      return "brief";
    case HtmlElement::UNCLOSED:
      return "unclosed";
    case HtmlElement::INVISIBLE:
      return "invisible";
  }
  return "?";
}

// DocType enum name. DocType::DocTypeEnum is private, so map through the
// public static constants; OTHER_XHTML is the elimination fallback
// (SPEC.md SS3.4).
inline const char* DoctypeName(const DocType& dt) {
  if (dt == DocType::kHTML5) return "HTML_5";
  if (dt == DocType::kHTML4Strict) return "HTML_4_STRICT";
  if (dt == DocType::kHTML4Transitional) return "HTML_4_TRANSITIONAL";
  if (dt == DocType::kXHTML5) return "XHTML_5";
  if (dt == DocType::kXHTML11) return "XHTML_1_1";
  if (dt == DocType::kXHTML10Strict) return "XHTML_1_0_STRICT";
  if (dt == DocType::kXHTML10Transitional) return "XHTML_1_0_TRANSITIONAL";
  if (dt == DocType::kUnknown) return "UNKNOWN";
  return "OTHER_XHTML";
}

// Recording event sink: renders each parse event as one line per SPEC.md
// SS3.3.
class RecordingHtmlFilter : public EmptyHtmlFilter {
 public:
  explicit RecordingHtmlFilter(GoogleString* out) : out_(out) {}
  ~RecordingHtmlFilter() override {}

  const char* Name() const override { return "HtmlParseRecorder"; }

  void StartDocument() override { out_->append("document-start\n"); }
  void EndDocument() override { out_->append("document-end\n"); }

  void StartElement(HtmlElement* element) override {
    out_->append("start ");
    AppendKeyword(element->keyword(), out_);
    *out_ += ' ';
    AppendSpelling(element->name(), out_);
    const HtmlElement::AttributeList& attrs = element->attributes();
    for (HtmlElement::AttributeConstIterator i(attrs.begin()); i != attrs.end();
         ++i) {
      const HtmlElement::Attribute& attr = *i;
      *out_ += ' ';
      AppendKeyword(attr.keyword(), out_);
      *out_ += ':';
      AppendSpelling(attr.name(), out_);
      *out_ += '=';
      *out_ += QuoteChar(attr.quote_style());
      const char* decoded = attr.DecodedValueOrNull();
      if (decoded == nullptr) {
        if (attr.escaped_value() == nullptr) {
          *out_ += '~';  // No value at all.
        } else {
          *out_ += '!';  // Decoding error: render the raw escaped bytes.
          AppendEscaped(StringPiece(attr.escaped_value()), out_);
        }
      } else {
        *out_ += 'v';
        AppendEscaped(StringPiece(decoded), out_);
      }
    }
    *out_ += '\n';
  }

  void EndElement(HtmlElement* element) override {
    out_->append("end ");
    AppendKeyword(element->keyword(), out_);
    *out_ += ' ';
    AppendSpelling(element->name(), out_);
    *out_ += ' ';
    out_->append(StyleName(element->style()));
    *out_ += '\n';
  }

  void Cdata(HtmlCdataNode* cdata) override {
    AppendLeaf("cdata", cdata, out_);
  }
  void Comment(HtmlCommentNode* comment) override {
    AppendLeaf("comment", comment, out_);
  }
  void IEDirective(HtmlIEDirectiveNode* directive) override {
    AppendLeaf("iedirective", directive, out_);
  }
  void Characters(HtmlCharactersNode* characters) override {
    AppendLeaf("chars", characters, out_);
  }
  void Directive(HtmlDirectiveNode* directive) override {
    AppendLeaf("directive", directive, out_);
  }

 private:
  GoogleString* out_;

  RecordingHtmlFilter(const RecordingHtmlFilter&) = delete;
  RecordingHtmlFilter& operator=(const RecordingHtmlFilter&) = delete;
};

// Drives one input through HtmlParse with a RecordingHtmlFilter, per
// SPEC.md SS2. Appends the complete stream (events + trailer) to *out.
// Returns kExitOk or kExitSizeLimit; kExitHarnessError if the fixed base
// URL is refused (a harness bug, never input-dependent).
inline int RunParseProbe(const char* data, size_t size, GoogleString* out) {
  NullMessageHandler handler;
  HtmlParse parse(&handler);
  RecordingHtmlFilter recorder(out);
  parse.AddFilter(&recorder);
  if (!parse.StartParseWithType(FixedBaseUrl(), kContentTypeHtml)) {
    return kExitHarnessError;
  }
  parse.ParseText(data, static_cast<int>(size));
  parse.FinishParse();
  out->append("doctype ");
  out->append(DoctypeName(parse.doctype()));
  *out += '\n';
  const bool size_limit = parse.size_limit_exceeded();
  out->append(size_limit ? "flags size-limit-exceeded\n" : "flags none\n");
  return size_limit ? kExitSizeLimit : kExitOk;
}

}  // namespace html_parse_probe
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTML_HTML_PARSE_RECORDER_H_

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/template.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"

namespace pagespeed::packs {
namespace {

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

absl::StatusOr<Template> ParseTemplate(std::string_view text,
                                       const TemplateParseOptions& options) {
  Template tpl;
  tpl.source = std::string(text);
  std::string literal;

  auto flush_literal = [&]() {
    if (literal.empty()) return;
    TemplateSegment seg;
    seg.is_literal = true;
    seg.literal = std::move(literal);
    tpl.segments.push_back(std::move(seg));
    literal.clear();
  };

  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '{') {
      literal.push_back(text[i++]);
      continue;
    }
    size_t j = i + 1;
    while (j < text.size() && IsIdentChar(text[j])) ++j;
    if (j == i + 1 || j >= text.size() || text[j] != '}') {
      literal.push_back(text[i++]);  // not a "{identifier}" token
      continue;
    }
    const std::string_view name = text.substr(i + 1, j - i - 1);

    TemplateSegment seg;
    seg.is_literal = false;
    bool page_value = false;
    if (name == "scheme") {
      seg.placeholder = Placeholder::kScheme;
    } else if (name == "host") {
      seg.placeholder = Placeholder::kHost;
    } else if (name == "path") {
      seg.placeholder = Placeholder::kPath;
    } else if (name == "query") {
      seg.placeholder = Placeholder::kQuery;
    } else if (name == "url") {
      seg.placeholder = Placeholder::kUrl;
    } else if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') {
      seg.placeholder = Placeholder::kCapture;
      seg.capture = name[0] - '0';
      if (seg.capture > options.capture_groups) {
        return absl::InvalidArgumentError(absl::StrCat(
            "placeholder {", name, "} refers to capture group ", seg.capture,
            " but path_regex has ", options.capture_groups,
            options.capture_groups == 1 ? " group" : " groups"));
      }
    } else if (name == "title") {
      seg.placeholder = Placeholder::kTitle;
      page_value = true;
    } else if (name == "description") {
      seg.placeholder = Placeholder::kDescription;
      page_value = true;
    } else if (name == "canonical") {
      seg.placeholder = Placeholder::kCanonical;
      page_value = true;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("unknown placeholder {", name, "}"));
    }
    if (page_value && !options.allow_page_values) {
      return absl::InvalidArgumentError(
          absl::StrCat("placeholder {", name, "} is only allowed in jsonld"));
    }
    flush_literal();
    tpl.segments.push_back(std::move(seg));
    i = j + 1;
  }
  flush_literal();
  return tpl;
}

std::string EscapeForHtmlText(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string EscapeJsonString(std::string_view s) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (u < 0x20 || u == 0x7f) {
          out += "\\u00";
          out.push_back(kHex[u >> 4]);
          out.push_back(kHex[u & 0xf]);
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

absl::StatusOr<std::string> ExpandTemplate(const Template& tpl,
                                           const ExpandContext& ctx,
                                           EscapeContext escape,
                                           size_t max_bytes) {
  std::string out;
  auto too_big = [&]() {
    return absl::ResourceExhaustedError(
        absl::StrCat("expanded value exceeds ", max_bytes, " bytes"));
  };

  // Appends one piece. Escaping never shrinks a piece, so the raw size is a
  // lower bound that is checked before anything is copied or escaped.
  auto append = [&](std::string_view piece, bool is_value) -> bool {
    if (piece.size() > max_bytes || out.size() + piece.size() > max_bytes) {
      return false;
    }
    if (escape == EscapeContext::kHtmlText) {
      out += EscapeForHtmlText(piece);
    } else if (escape == EscapeContext::kJsonString && is_value) {
      out += EscapeJsonString(piece);
    } else {
      out += piece;
    }
    return out.size() <= max_bytes;
  };

  for (const TemplateSegment& seg : tpl.segments) {
    if (seg.is_literal) {
      if (!append(seg.literal, false)) return too_big();
      continue;
    }
    bool ok = true;
    switch (seg.placeholder) {
      case Placeholder::kScheme:
        ok = append(ctx.url.scheme, true);
        break;
      case Placeholder::kHost:
        ok = append(ctx.url.host, true);
        break;
      case Placeholder::kPath:
        ok = append(ctx.url.path, true);
        break;
      case Placeholder::kQuery:
        if (!ctx.url.query.empty()) {
          ok = append("?", true) && append(ctx.url.query, true);
        }
        break;
      case Placeholder::kUrl:
        ok = append(ctx.url.scheme, true) && append("://", true) &&
             append(ctx.url.host, true) && append(ctx.url.path, true);
        if (ok && !ctx.url.query.empty()) {
          ok = append("?", true) && append(ctx.url.query, true);
        }
        break;
      case Placeholder::kCapture: {
        const size_t idx = static_cast<size_t>(seg.capture) - 1;
        if (idx < ctx.captures.size()) ok = append(ctx.captures[idx], true);
        break;
      }
      case Placeholder::kTitle:
        ok = append(ctx.title, true);
        break;
      case Placeholder::kDescription:
        ok = append(ctx.description, true);
        break;
      case Placeholder::kCanonical:
        ok = append(ctx.canonical, true);
        break;
    }
    if (!ok) return too_big();
  }

  if (escape == EscapeContext::kJsonString) {
    // '<' grows 1 -> 6 bytes; count first so nothing big is built.
    const size_t lt =
        static_cast<size_t>(std::count(out.begin(), out.end(), '<'));
    if (out.size() + lt * 5 > max_bytes) return too_big();
    std::string final_out;
    final_out.reserve(out.size() + lt * 5);
    for (char c : out) {
      if (c == '<') {
        final_out += "\\u003c";
      } else {
        final_out.push_back(c);
      }
    }
    out = std::move(final_out);
  }
  return out;
}

}  // namespace pagespeed::packs

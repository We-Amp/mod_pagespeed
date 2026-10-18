// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/link_header.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {
namespace {

bool IsOws(char c) { return c == ' ' || c == '\t'; }

void SkipOws(std::string_view s, size_t* i) {
  while (*i < s.size() && IsOws(s[*i])) ++*i;
}

// Advances `*i` past the next top-level comma (or to the end), skipping
// quoted strings and `<...>`.
void SkipToNextValue(std::string_view s, size_t* i) {
  bool in_quote = false;
  bool in_angle = false;
  for (; *i < s.size(); ++*i) {
    const char c = s[*i];
    if (in_quote) {
      if (c == '\\' && *i + 1 < s.size()) {
        ++*i;
      } else if (c == '"') {
        in_quote = false;
      }
    } else if (in_angle) {
      if (c == '>') in_angle = false;
    } else if (c == '"') {
      in_quote = true;
    } else if (c == '<') {
      in_angle = true;
    } else if (c == ',') {
      ++*i;
      return;
    }
  }
}

// Reads a quoted string whose opening quote is at s[*i]. Returns false when
// it is not terminated.
bool ReadQuoted(std::string_view s, size_t* i, std::string* out) {
  ++*i;  // opening quote
  while (*i < s.size()) {
    const char c = s[*i];
    if (c == '\\' && *i + 1 < s.size()) {
      out->push_back(s[*i + 1]);
      *i += 2;
    } else if (c == '"') {
      ++*i;
      return true;
    } else {
      out->push_back(c);
      ++*i;
    }
  }
  return false;
}

// Parses one link-value starting at s[*i] == '<'. On success leaves `*i` at
// the character that ends it (a top-level comma or the end).
bool ParseOne(std::string_view s, size_t* i, LinkValue* out) {
  const size_t start = *i;
  const size_t close = s.find('>', *i + 1);
  if (close == std::string_view::npos) return false;
  out->target = std::string(s.substr(*i + 1, close - *i - 1));
  *i = close + 1;
  size_t end = *i;  // end of the last well-formed part
  for (;;) {
    SkipOws(s, i);
    if (*i >= s.size() || s[*i] == ',') break;
    if (s[*i] != ';') return false;
    ++*i;
    SkipOws(s, i);
    const size_t name_start = *i;
    while (*i < s.size() && s[*i] != '=' && s[*i] != ';' && s[*i] != ',' &&
           !IsOws(s[*i])) {
      ++*i;
    }
    std::string name =
        absl::AsciiStrToLower(s.substr(name_start, *i - name_start));
    std::string value;
    SkipOws(s, i);
    if (*i < s.size() && s[*i] == '=') {
      ++*i;
      SkipOws(s, i);
      if (*i < s.size() && s[*i] == '"') {
        if (!ReadQuoted(s, i, &value)) return false;
      } else {
        const size_t value_start = *i;
        while (*i < s.size() && s[*i] != ';' && s[*i] != ',' && !IsOws(s[*i])) {
          ++*i;
        }
        value = std::string(s.substr(value_start, *i - value_start));
      }
    }
    if (!name.empty())
      out->params.emplace_back(std::move(name), std::move(value));
    end = *i;
  }
  out->raw = std::string(s.substr(start, end - start));
  return true;
}

}  // namespace

std::optional<std::string> LinkValue::Param(std::string_view name) const {
  for (const auto& [n, v] : params) {
    if (absl::EqualsIgnoreCase(n, name)) return v;
  }
  return std::nullopt;
}

bool LinkValue::HasRel(std::string_view token) const {
  // A link-value can carry several rel parameters; every one of them
  // counts, in any order.
  for (const auto& [n, v] : params) {
    if (!absl::EqualsIgnoreCase(n, "rel")) continue;
    for (std::string_view t :
         absl::StrSplit(v, absl::ByAnyChar(" \t"), absl::SkipEmpty())) {
      if (absl::EqualsIgnoreCase(t, token)) return true;
    }
  }
  return false;
}

std::vector<LinkValue> SplitLinkHeader(std::string_view field_value,
                                       bool* clean) {
  if (clean != nullptr) *clean = true;
  std::vector<LinkValue> out;
  size_t i = 0;
  while (i < field_value.size()) {
    SkipOws(field_value, &i);
    if (i >= field_value.size()) break;
    if (field_value[i] == ',') {
      ++i;
      continue;
    }
    LinkValue value;
    if (field_value[i] == '<') {
      size_t at = i;
      if (ParseOne(field_value, &at, &value)) {
        out.push_back(std::move(value));
        i = at;
        continue;
      }
    }
    if (clean != nullptr) *clean = false;
    SkipToNextValue(field_value, &i);
  }
  return out;
}

std::string EncodeLinkTarget(std::string_view url) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(url.size());
  for (char ch : url) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c == 0x7F || c > 0x7E || c == '<' || c == '>' ||
        c == '"' || c == ',' || c == ';' || c == '\\') {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::optional<std::string> MakeCanonicalLinkValue(std::string_view url) {
  if (url.empty() || url.size() > kMaxUrlBytes) return std::nullopt;
  std::string target = EncodeLinkTarget(url);
  if (target.size() > kMaxUrlBytes) return std::nullopt;
  return absl::StrCat("<", target, ">; rel=\"canonical\"");
}

std::string LinkValueWithoutRelToken(const LinkValue& value,
                                     std::string_view token) {
  if (!value.HasRel(token)) return value.raw;
  // The token is dropped from the merged tokens of every rel parameter;
  // the merged set is written where the first rel parameter stood and the
  // other rel parameters are not repeated.
  std::string kept;
  for (const auto& [name, param] : value.params) {
    if (name != "rel") continue;
    for (std::string_view t :
         absl::StrSplit(param, absl::ByAnyChar(" \t"), absl::SkipEmpty())) {
      if (absl::EqualsIgnoreCase(t, token)) continue;
      if (!kept.empty()) kept.push_back(' ');
      kept.append(t);
    }
  }
  if (kept.empty()) return std::string();
  std::string out = absl::StrCat("<", value.target, ">");
  bool rel_written = false;
  for (const auto& [name, param] : value.params) {
    if (name == "rel") {
      if (!rel_written) {
        rel_written = true;
        absl::StrAppend(&out, "; rel=\"", kept, "\"");
      }
      continue;
    }
    if (param.empty()) {
      absl::StrAppend(&out, "; ", name);
      continue;
    }
    if (name.back() == '*') {
      // An RFC 8187 extended value is a token, never a quoted string.
      absl::StrAppend(&out, "; ", name, "=", param);
      continue;
    }
    std::string quoted;
    for (char c : param) {
      if (c == '"' || c == '\\') quoted.push_back('\\');
      quoted.push_back(c);
    }
    absl::StrAppend(&out, "; ", name, "=\"", quoted, "\"");
  }
  return out;
}

namespace {

// How a link target compares to the document's canonical; empty when it is
// not an absolute http(s) URL once resolved.
std::string TargetNorm(std::string_view page_url, std::string_view target) {
  return NormUrl(ResolveUrl(page_url, CollapseWhitespace(target)))
      .value_or(std::string());
}

}  // namespace

CanonicalLinkEdit PlanCanonicalLinkHeader(const std::vector<std::string>& lines,
                                          std::string_view page_url,
                                          std::string_view final_norm,
                                          std::string_view final_url,
                                          std::string_view target_norm,
                                          OnPresent on_present) {
  CanonicalLinkEdit edit;
  if (final_norm.empty() || final_url.empty()) return edit;

  struct Entry {
    size_t line = 0;
    LinkValue value;
    bool canonical = false;
    bool conflicting = false;
  };
  std::vector<Entry> entries;
  std::vector<bool> clean(lines.size(), true);
  size_t canonicals = 0;
  size_t conflicts = 0;
  size_t equal = 0;
  std::string first_conflict;
  for (size_t i = 0; i < lines.size(); ++i) {
    bool line_clean = true;
    for (LinkValue& v : SplitLinkHeader(lines[i], &line_clean)) {
      Entry e;
      e.line = i;
      e.canonical = v.HasRel("canonical");
      if (e.canonical) {
        ++canonicals;
        const std::string norm = TargetNorm(page_url, v.target);
        e.conflicting = norm != final_norm;
        if (e.conflicting) {
          ++conflicts;
          if (first_conflict.empty()) {
            first_conflict = norm.empty() ? v.target : norm;
          }
        } else {
          ++equal;
        }
      }
      e.value = std::move(v);
      entries.push_back(std::move(e));
    }
    clean[i] = line_clean;
  }

  const std::optional<std::string> added = MakeCanonicalLinkValue(final_url);
  if (canonicals == 0) {
    // The document's canonical is added only when it is the rule's own value.
    if (final_norm != target_norm || !added.has_value()) return edit;
    edit.applicable = true;
    edit.action = Action::kInsert;
    edit.reason = Reason::kAbsent;
    edit.mutate = true;
    edit.append_only = true;
    edit.lines = lines;
    edit.lines.push_back(*added);
    return edit;
  }
  edit.applicable = true;
  if (conflicts == 0) {
    edit.action = Action::kNone;
    edit.reason = Reason::kEqual;
    return edit;
  }
  edit.reason = Reason::kHeaderConflict;
  edit.defect = "canonical-header-conflict";
  edit.before_hash = HashValue(first_conflict);
  if (on_present == OnPresent::kKeep) {
    edit.action = Action::kNone;
    return edit;
  }
  if (equal == 0 && !added.has_value()) {
    // The document's canonical cannot be written as a link-value, so the
    // header is left as the origin sent it.
    edit.action = Action::kNone;
    return edit;
  }
  // A conflicting entry on a line the splitter could not read in full cannot
  // be taken out without dropping the text it skipped. The conflict is
  // reported and the header is left as the origin sent it.
  for (const Entry& e : entries) {
    if (e.conflicting && !clean[e.line]) {
      edit.action = Action::kNone;
      return edit;
    }
  }
  edit.action = Action::kReplace;
  edit.mutate = true;
  // Rewrite only the lines that carry a conflicting canonical; every other
  // link-value of such a line stays, as does any line without one.
  std::vector<bool> touched(lines.size(), false);
  for (const Entry& e : entries) {
    if (e.conflicting) touched[e.line] = true;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!touched[i]) {
      edit.lines.push_back(lines[i]);
      continue;
    }
    std::string rewritten;
    for (const Entry& e : entries) {
      if (e.line != i) continue;
      std::string piece = e.value.raw;
      if (e.conflicting) {
        // Keep the link-value when it also says something else (rel="canonical
        // preload"); drop only the canonical token.
        const std::string rest = LinkValueWithoutRelToken(e.value, "canonical");
        if (rest.empty()) continue;
        piece = rest;
      }
      if (!rewritten.empty()) rewritten.append(", ");
      rewritten.append(piece);
    }
    if (!rewritten.empty()) edit.lines.push_back(std::move(rewritten));
  }
  if (equal == 0) edit.lines.push_back(*added);
  return edit;
}

}  // namespace pagespeed::packs

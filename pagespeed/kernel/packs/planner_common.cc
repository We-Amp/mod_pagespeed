// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/planner_common.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/packs/entity_decode.h"

namespace pagespeed::packs {

std::string_view SkipReasonName(SkipReason reason) {
  switch (reason) {
    case SkipReason::kNone:
      return "none";
    case SkipReason::kHostNotListed:
      return "host_not_listed";
    case SkipReason::kNoHead:
      return "no_head";
    case SkipReason::kMalformedHead:
      return "malformed_head";
    case SkipReason::kNotRewritable:
      return "not_rewritable";
    case SkipReason::kSizeLimit:
      return "size_limit";
  }
  return "unknown";
}

namespace {

bool IsWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool HasScheme(std::string_view s) {
  if (s.empty() || !absl::ascii_isalpha(static_cast<unsigned char>(s[0]))) {
    return false;
  }
  for (size_t i = 1; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ':') return true;
    if (!(absl::ascii_isalnum(c) || c == '+' || c == '-' || c == '.')) {
      return false;
    }
  }
  return false;
}

// Removes "." and ".." segments from the path part of `s` (before any ? or
// #), leaving the rest untouched.
std::string RemoveDotSegments(std::string_view s) {
  const size_t tail_at = s.find_first_of("?#");
  std::string_view path = s.substr(0, tail_at);
  std::string_view tail = tail_at == std::string_view::npos ? std::string_view()
                                                            : s.substr(tail_at);
  std::vector<std::string_view> out;
  std::vector<std::string_view> parts = absl::StrSplit(path, '/');
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string_view p = parts[i];
    const bool last = i + 1 == parts.size();
    if (p == ".") {
      if (last) out.push_back("");
    } else if (p == "..") {
      if (out.size() > 1) out.pop_back();
      if (last) out.push_back("");
    } else {
      out.push_back(p);
    }
  }
  return absl::StrCat(absl::StrJoin(out, "/"), tail);
}

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

std::string EscapeAttributeValue(std::string_view s) {
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
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string CollapseWhitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (char c : s) {
    if (IsWs(c)) {
      pending_space = !out.empty();
    } else {
      if (pending_space) out.push_back(' ');
      pending_space = false;
      out.push_back(c);
    }
  }
  return out;
}

std::string ResolveUrl(std::string_view base, std::string_view ref) {
  while (!ref.empty() && IsWs(ref.front())) ref.remove_prefix(1);
  while (!ref.empty() && IsWs(ref.back())) ref.remove_suffix(1);
  if (HasScheme(ref)) return std::string(ref);
  const size_t scheme_end = base.find("://");
  if (scheme_end == std::string_view::npos) return std::string();
  const size_t frag = base.find('#');
  if (frag != std::string_view::npos) base = base.substr(0, frag);
  size_t auth_end = base.find_first_of("/?", scheme_end + 3);
  if (auth_end == std::string_view::npos) auth_end = base.size();
  const std::string_view origin = base.substr(0, auth_end);
  const std::string_view rest = base.substr(auth_end);
  const size_t q = rest.find('?');
  std::string_view path = rest.substr(0, q);
  std::string_view query =
      q == std::string_view::npos ? std::string_view() : rest.substr(q);
  if (path.empty()) path = "/";
  if (ref.empty()) return absl::StrCat(origin, path, query);
  if (ref.starts_with("//")) {
    return absl::StrCat(base.substr(0, scheme_end + 1), ref);
  }
  if (ref.front() == '#') return absl::StrCat(origin, path, query, ref);
  if (ref.front() == '?') return absl::StrCat(origin, path, ref);
  if (ref.front() == '/') return absl::StrCat(origin, RemoveDotSegments(ref));
  const size_t slash = path.rfind('/');
  return absl::StrCat(
      origin, RemoveDotSegments(absl::StrCat(path.substr(0, slash + 1), ref)));
}

std::string ReplaceInvalidUtf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    uint32_t min = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
      out.push_back(s[i++]);
      continue;
    } else if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
      min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
      min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
      min = 0x10000;
    }
    bool ok = len != 0 && i + len <= s.size();
    for (size_t k = 1; ok && k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      ok = (cc & 0xC0) == 0x80;
      cp = (cp << 6) | (cc & 0x3F);
    }
    ok = ok && cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
    if (ok) {
      out.append(s.substr(i, len));
      i += len;
    } else {
      out += "\xEF\xBF\xBD";  // U+FFFD
      ++i;
    }
  }
  return out;
}

std::string DecodeAttribute(std::string_view escaped) {
  std::string out;
  size_t seg = 0;
  size_t i = 0;
  while (i < escaped.size()) {
    if (escaped[i] == '&' && i + 2 < escaped.size() && escaped[i + 1] == '#') {
      size_t j = i + 2;
      const bool hex = escaped[j] == 'x' || escaped[j] == 'X';
      if (hex) ++j;
      const size_t digits_at = j;
      uint32_t cp = 0;
      while (j < escaped.size() && j - digits_at < 8) {
        const unsigned char d = static_cast<unsigned char>(escaped[j]);
        uint32_t v;
        if (absl::ascii_isdigit(d)) {
          v = d - '0';
        } else if (hex && absl::ascii_isxdigit(d)) {
          v = absl::ascii_tolower(d) - 'a' + 10;
        } else {
          break;
        }
        cp = cp * (hex ? 16 : 10) + v;
        ++j;
      }
      if (j > digits_at && j < escaped.size() && escaped[j] == ';' &&
          cp >= 0x20 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
        out += SafeDecodeHtmlEntities(escaped.substr(seg, i - seg));
        AppendUtf8(cp, &out);
        i = seg = j + 1;
        continue;
      }
    }
    ++i;
  }
  out += SafeDecodeHtmlEntities(escaped.substr(seg));
  return out;
}

std::string AttrValue(const net_instaweb::HtmlElement* element,
                      net_instaweb::HtmlName::Keyword keyword, bool* present) {
  // The kernel's own decoded value is empty for non-ASCII bytes and some
  // entities, so the escaped value is read and decoded here.
  const char* escaped = element->EscapedAttributeValue(keyword);
  if (present != nullptr) *present = escaped != nullptr;
  if (escaped == nullptr) return std::string();
  return DecodeAttribute(escaped);
}

void Finish(RulePlan* plan, Action action, Reason reason, std::string defect) {
  plan->decision.action = action;
  plan->decision.reason = reason;
  plan->decision.defect = std::move(defect);
}

void SummarizeOutcome(RulePlan* plan, const OutcomeFlags& f) {
  const bool acts = f.removed || f.set || f.inserted;
  if (!acts) {
    Finish(plan, Action::kNone, f.idle_reason, f.idle_defect);
    return;
  }
  Action action;
  if (f.set || (f.inserted && f.removed)) {
    action = Action::kReplace;
  } else if (f.inserted) {
    action = Action::kInsert;
  } else {
    action = f.duplicate && !f.conflict && !f.has_unusable ? Action::kDedupe
                                                           : Action::kRemove;
  }
  if (f.has_unusable) {
    Finish(plan, action, f.unusable_reason, f.unusable_defect);
  } else if (f.conflict) {
    Finish(plan, action, Reason::kConflict, std::string());
  } else if (f.self_missing && (f.inserted || f.set)) {
    Finish(plan, action, Reason::kAbsent, f.self_defect);
  } else if (f.set) {
    Finish(plan, action, Reason::kPresent, std::string());
  } else if (f.inserted) {
    Finish(plan, action, Reason::kAbsent,
           f.had_any ? std::string() : f.absent_defect);
  } else {
    Finish(plan, action, Reason::kDuplicate, std::string());
  }
}

void NoteRemoved(RulePlan* plan, std::string_view item,
                 std::string_view reason) {
  if (!plan->decision.removed.empty()) plan->decision.removed += "; ";
  absl::StrAppend(&plan->decision.removed, item, " (", reason, ")");
}

void CapRemoved(RulePlan* plan, size_t total) {
  std::string& r = plan->decision.removed;
  if (r.size() <= kMaxAfterBytes) return;
  const std::string marker_room = "; ...(+9999)";
  const std::string_view head =
      std::string_view(r).substr(0, kMaxAfterBytes - marker_room.size());
  size_t cut = head.rfind("; ");
  size_t kept = 0;
  std::string out;
  if (cut != std::string_view::npos) {
    out = std::string(head.substr(0, cut));
    for (size_t at = out.find("; "); at != std::string::npos;
         at = out.find("; ", at + 2)) {
      ++kept;
    }
    ++kept;
  }
  out = TruncateUtf8(out, kMaxAfterBytes);
  absl::StrAppend(&out, out.empty() ? "" : "; ", "...(+", total - kept, ")");
  r = std::move(out);
}

size_t AddedBytes(const RulePlan& plan, size_t extra) {
  size_t n = 0;
  for (const PlanOp& op : plan.ops) {
    if (op.type != OpType::kInsert && op.type != OpType::kSetValue) continue;
    // Text content is written as given (it is escaped already); attribute
    // values are escaped on the way out.
    size_t value = op.attr == net_instaweb::HtmlName::kNotAKeyword
                       ? op.value.size()
                       : EscapeAttributeValue(op.value).size();
    // (The code of an hreflang link; the debug comment's room, `extra`, is
    // added per entry, so a cluster counts it once per link although the
    // filter writes one comment per rule: an overcount, on the safe side.)
    if (!op.code.empty()) value += EscapeAttributeValue(op.code).size() + 32;
    n += value + extra + (op.type == OpType::kInsert ? 64 : 0);
  }
  return n;
}

}  // namespace pagespeed::packs

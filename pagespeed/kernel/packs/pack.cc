// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/pack.h"

#include <algorithm>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {

std::string_view ModeName(Mode mode) {
  return mode == Mode::kEnforce ? "enforce" : "report";
}

std::string_view OnPresentName(OnPresent on_present) {
  switch (on_present) {
    case OnPresent::kKeep:
      return "keep";
    case OnPresent::kRepair:
      return "repair";
    case OnPresent::kReplace:
      return "replace";
  }
  return "";
}

std::string_view LinkHeaderModeName(LinkHeaderMode mode) {
  return mode == LinkHeaderMode::kMatch ? "match" : "off";
}

Mode EffectiveMode(Mode global, Mode site, bool rule_enforce) {
  const Mode rule = rule_enforce ? Mode::kEnforce : Mode::kReport;
  return std::min({global, site, rule});
}

absl::Status CheckUrlValue(std::string_view value) {
  if (value.empty()) return absl::InvalidArgumentError("URL is empty");
  if (value.size() > kMaxUrlBytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("URL is longer than ", kMaxUrlBytes, " bytes"));
  }
  for (char c : value) {
    if (static_cast<unsigned char>(c) <= 0x20 ||
        static_cast<unsigned char>(c) == 0x7f) {
      return absl::InvalidArgumentError(
          "URL contains whitespace or a control character");
    }
    if (c == '"' || c == '<' || c == '>' || c == '\\') {
      return absl::InvalidArgumentError(
          "URL contains a backslash or one of the characters \" < >");
    }
  }
  if (!NormUrl(value).has_value()) {
    return absl::InvalidArgumentError(
        "URL must be absolute (http:// or https://) with a host");
  }
  return absl::OkStatus();
}

RE2::Options MakeRegexOptions() {
  RE2::Options options;
  options.set_encoding(RE2::Options::EncodingLatin1);
  options.set_max_mem(kRegexMaxMemBytes);
  options.set_log_errors(false);
  return options;
}

bool HasControlChars(std::string_view s) {
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return true;
  }
  return false;
}

bool JsonNestingExceeds(std::string_view s, size_t max_depth) {
  size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char c : s) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
    } else if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > max_depth) return true;
    } else if ((c == ']' || c == '}') && depth > 0) {
      --depth;
    }
  }
  return false;
}

size_t Utf8Length(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

}  // namespace pagespeed::packs

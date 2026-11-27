// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/origin_cache_control.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

// ASCII case-insensitive equality against a directive name.  Folds only A-Z,
// so directive matching cannot change meaning with the process locale.
bool AsciiEqualsIgnoreCase(std::string_view token, std::string_view name) {
  if (token.size() != name.size()) return false;
  for (size_t i = 0; i < token.size(); ++i) {
    auto ct = static_cast<unsigned char>(token[i]);
    auto cn = static_cast<unsigned char>(name[i]);
    if (ct >= 'A' && ct <= 'Z') ct = static_cast<unsigned char>(ct | 0x20);
    if (cn >= 'A' && cn <= 'Z') cn = static_cast<unsigned char>(cn | 0x20);
    if (ct != cn) return false;
  }
  return true;
}

// HTTP optional whitespace (RFC 9110 section 5.6.3).  The field syntax allows
// it around each list element; it is also tolerated around '=' and inside the
// quotes of a value, which the grammar does not allow.
bool IsOws(char c) { return c == ' ' || c == '\t'; }

std::string_view TrimOws(std::string_view s) {
  while (!s.empty() && IsOws(s.front())) s.remove_prefix(1);
  while (!s.empty() && IsOws(s.back())) s.remove_suffix(1);
  return s;
}

}  // namespace

uint32_t ParseCacheControlSeconds(std::string_view val) {
  // Skip surrounding optional whitespace.
  val = TrimOws(val);
  // Strip quotes.
  if (!val.empty() && val.front() == '"') val.remove_prefix(1);
  if (!val.empty() && val.back() == '"') val.remove_suffix(1);
  val = TrimOws(val);
  if (val.empty()) return 0;
  uint64_t result = 0;
  for (char c : val) {
    if (c < '0' || c > '9') return 0;  // Non-numeric.
    result = result * 10 + static_cast<uint64_t>(c - '0');
    if (result > UINT32_MAX) return UINT32_MAX;
  }
  return static_cast<uint32_t>(result);
}

void AccumulateOriginCacheControl(std::string_view header_value,
                                  OriginCacheControl* out) {
  if (out == nullptr) return;

  // The header line was present, whatever it turns out to say.  A present
  // but empty Cache-Control still answers "the origin sent one".
  out->cc_flags |= AlternateMetadata::kCCOriginHeaderPresent;

  std::string_view val(header_value);

  // Split on ',' and parse each directive.
  while (!val.empty()) {
    size_t comma = val.find(',');
    std::string_view token;
    if (comma == std::string_view::npos) {
      token = val;
      val = {};
    } else {
      token = val.substr(0, comma);
      val.remove_prefix(comma + 1);
    }
    // Trim optional whitespace.
    token = TrimOws(token);
    if (token.empty()) continue;

    // Split name=value.
    std::string_view name = token;
    std::string_view value;
    size_t eq = token.find('=');
    if (eq != std::string_view::npos) {
      name = token.substr(0, eq);
      value = token.substr(eq + 1);
      name = TrimOws(name);
    }

    // Table-driven Cache-Control directive lookup.
    static constexpr struct {
      std::string_view name;
      uint16_t flag;
    } kCCDirectives[] = {
        {"no-cache", AlternateMetadata::kCCOriginNoCache},
        {"must-revalidate", AlternateMetadata::kCCOriginMustRevalidate},
        {"proxy-revalidate", AlternateMetadata::kCCOriginProxyRevalidate},
        {"no-store", AlternateMetadata::kCCOriginNoStore},
        {"private", AlternateMetadata::kCCOriginPrivate},
        {"public", AlternateMetadata::kCCOriginPublic},
        {"immutable", AlternateMetadata::kCCOriginImmutable},
        {"no-transform", AlternateMetadata::kCCOriginNoTransform},
    };
    // Does this occurrence carry an argument?  `private="Set-Cookie"` is
    // the QUALIFIED form (RFC 9111 §5.2.2.7): only the listed field-names
    // are restricted, the remainder MAY be stored.
    //
    // The test is the PRESENCE of '=', not a non-empty value: §5.2.2.7's
    // ABNF permits an empty field-name list, so `private=""` is a legal
    // qualified form that restricts nothing — and must not be mistaken
    // for the blanket directive.
    //
    // The comma split above is not quoted-string aware, but that cannot
    // corrupt this classification: `private="A, B"` arrives as
    // `private="A` (still has '=', still qualified), and a stranded
    // fragment like `B"` matches no directive name at all.  Critically,
    // the split can never manufacture a BARE `private`/`no-cache` token
    // out of a quoted field list, which is what the bare bit relies on.
    const bool has_argument = (eq != std::string_view::npos);

    bool matched = false;
    for (const auto& d : kCCDirectives) {
      if (AsciiEqualsIgnoreCase(name, d.name)) {
        out->cc_flags |= d.flag;
        // Record the FORM of this occurrence.  Both directions are
        // tracked because cc_flags is OR-accumulated across every token
        // of every header line: "bare seen" is monotone under OR and is
        // what a destructive consumer must key on, while "qualified
        // seen" only distinguishes a parsed qualified-only response from
        // legacy metadata that predates both bits.  See
        // alternate_metadata.h for the full decision rule.
        if (d.flag == AlternateMetadata::kCCOriginPrivate) {
          out->cc_flags |= has_argument
                               ? AlternateMetadata::kCCOriginPrivateQualified
                               : AlternateMetadata::kCCOriginPrivateBare;
        } else if (d.flag == AlternateMetadata::kCCOriginNoCache) {
          out->cc_flags |= has_argument
                               ? AlternateMetadata::kCCOriginNoCacheQualified
                               : AlternateMetadata::kCCOriginNoCacheBare;
        }
        matched = true;
        break;
      }
    }
    if (!matched) {
      if (AsciiEqualsIgnoreCase(name, "max-age")) {
        out->max_age = ParseCacheControlSeconds(value);
      } else if (AsciiEqualsIgnoreCase(name, "s-maxage")) {
        out->s_maxage = ParseCacheControlSeconds(value);
        if ((out->cc_flags & AlternateMetadata::kCCOriginSMaxagePresent) != 0) {
          out->cc_flags |= AlternateMetadata::kCCOriginSMaxageRepeated;
        }
        out->cc_flags |= AlternateMetadata::kCCOriginSMaxagePresent;
      }
    }
  }
}

}  // namespace pagespeed

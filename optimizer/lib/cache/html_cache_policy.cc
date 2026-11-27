// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/html_cache_policy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;

// The optimizer's own rewritten URLs carry this marker in the file name.
constexpr std::string_view kRewrittenUrlMarker = ".pagespeed.";

// ASCII-only folds, so nothing here changes meaning with the process locale.
char AsciiLowerChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= 'A' && u <= 'Z') ? static_cast<char>(u | 0x20) : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiLowerChar(a[i]) != AsciiLowerChar(b[i])) return false;
  }
  return true;
}

bool StartsWithIgnoreCase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         EqualsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  const size_t last = haystack.size() - needle.size();
  for (size_t i = 0; i <= last; ++i) {
    if (EqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = AsciiLowerChar(c);
  return out;
}

// RFC 9110 §5.6.3 optional whitespace: SP and HTAB.
std::string_view TrimOws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

// A detail string safe for a log line: token characters only (what a header
// name or a Vary token is made of), bounded.  Anything else becomes `_`, so
// the result is always a token the decision line's grammar admits.
std::string SanitizeDetail(std::string_view s) {
  std::string out;
  out.reserve(std::min(s.size(), kMaxHtmlStoreDetailBytes));
  for (char c : s) {
    if (out.size() >= kMaxHtmlStoreDetailBytes) break;
    out.push_back(IsHttpTokenChar(c) ? c : '_');
  }
  return out;
}

const HeaderField* FindHeader(std::span<const HeaderField> headers,
                              std::string_view name) {
  for (const HeaderField& h : headers) {
    if (EqualsIgnoreCase(h.name, name)) return &h;
  }
  return nullptr;
}

size_t CountHeader(std::span<const HeaderField> headers,
                   std::string_view name) {
  size_t n = 0;
  for (const HeaderField& h : headers) {
    if (EqualsIgnoreCase(h.name, name)) ++n;
  }
  return n;
}

bool ParseDigits(std::string_view s, unsigned* out) {
  unsigned v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  *out = v;
  return true;
}

// How many times does this Cache-Control line carry the directive `name`?
// The shared parser keeps the last max-age / s-maxage it sees and does not
// say whether there were several; RFC 9111 section 4.2.1 says a cache that
// meets conflicting lifetimes uses the first or treats the response as stale.
// This policy treats it as stale, so it has to count them.
size_t CountDirective(std::string_view line, std::string_view name) {
  size_t count = 0;
  while (!line.empty()) {
    const size_t comma = line.find(',');
    std::string_view token = line.substr(0, comma);
    line = comma == std::string_view::npos ? std::string_view()
                                           : line.substr(comma + 1);
    const size_t eq = token.find('=');
    token = TrimOws(token.substr(0, eq));
    if (EqualsIgnoreCase(token, name)) ++count;
  }
  return count;
}

HtmlStoreVerdict Refuse(HtmlStoreVerdict v, HtmlStoreReason reason,
                        std::string detail = std::string()) {
  v.action = HtmlStoreAction::kRefuse;
  v.reason = reason;
  v.detail = std::move(detail);
  return v;
}

// ---- date arithmetic -----------------------------------------------------

// Days from 1970-01-01 to the given civil date (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool IsLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned DaysInMonth(int64_t y, unsigned m) {
  static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31};
  return (m == 2 && IsLeap(y)) ? 29 : kDays[m - 1];
}

uint32_t SaturateU32(int64_t v) {
  if (v <= 0) return 0;
  if (v > static_cast<int64_t>(UINT32_MAX)) return UINT32_MAX;
  return static_cast<uint32_t>(v);
}

// ---- personalisation scan ------------------------------------------------

constexpr size_t kMaxTagBytes = 4096;

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// The text of the tag starting at body[pos] == '<', through its closing '>',
// bounded by kMaxTagBytes.  A '>' inside a quoted attribute value does not
// close the tag.  `*unclosed` says no closing '>' was found, so its attributes
// may not all have been seen.
std::string_view TagText(std::string_view body, size_t pos, bool* unclosed) {
  *unclosed = false;
  const size_t limit = std::min(body.size(), pos + kMaxTagBytes);
  char quote = 0;
  for (size_t i = pos + 1; i < limit; ++i) {
    const char c = body[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return body.substr(pos, i - pos + 1);
    }
  }
  *unclosed = true;
  return body.substr(pos, limit - pos);
}

// The value of attribute `name` in `tag` (the whole tag text, starting at
// '<'), or nullopt.  `tag_name_len` is the length of "<meta" and friends.
std::optional<std::string_view> FindAttribute(std::string_view tag,
                                              size_t tag_name_len,
                                              std::string_view name) {
  size_t i = tag_name_len;
  const size_t n = tag.size();
  while (i < n) {
    while (i < n && (IsAsciiSpace(tag[i]) || tag[i] == '/')) ++i;
    if (i >= n || tag[i] == '>') return std::nullopt;
    const size_t name_start = i;
    while (i < n && !IsAsciiSpace(tag[i]) && tag[i] != '=' && tag[i] != '>' &&
           tag[i] != '/') {
      ++i;
    }
    const std::string_view attr = tag.substr(name_start, i - name_start);
    while (i < n && IsAsciiSpace(tag[i])) ++i;
    std::string_view value;
    if (i < n && tag[i] == '=') {
      ++i;
      while (i < n && IsAsciiSpace(tag[i])) ++i;
      if (i < n && (tag[i] == '"' || tag[i] == '\'')) {
        const char q = tag[i++];
        const size_t v_start = i;
        while (i < n && tag[i] != q) ++i;
        value = tag.substr(v_start, i - v_start);
        if (i < n) ++i;
      } else {
        const size_t v_start = i;
        while (i < n && !IsAsciiSpace(tag[i]) && tag[i] != '>') ++i;
        value = tag.substr(v_start, i - v_start);
      }
    }
    if (!attr.empty() && EqualsIgnoreCase(attr, name)) return value;
  }
  return std::nullopt;
}

// True when `tag` begins with `<name` followed by a character that ends a tag
// name, so `<inputs` and `<formal` are not tags of interest.
bool IsTagNamed(std::string_view tag, std::string_view name_with_bracket) {
  if (!StartsWithIgnoreCase(tag, name_with_bracket)) return false;
  if (tag.size() == name_with_bracket.size()) return true;
  const char next = tag[name_with_bracket.size()];
  return IsAsciiSpace(next) || next == '/' || next == '>';
}

constexpr std::array<std::string_view, 11> kSignalNames = {
    "meta_csrf_token",
    "meta_csrf_param",
    "hidden_token",
    "hidden_csrf",
    "hidden_authenticity",
    "hidden_request_verification",
    "hidden_csrfmiddlewaretoken",
    "hidden_wpnonce",
    "post_form_login",
    "post_form_checkout",
    "oversized_tag"};

std::optional<std::string_view> SignalForTag(std::string_view tag) {
  if (IsTagNamed(tag, "<meta")) {
    const auto name = FindAttribute(tag, 5, "name");
    if (name.has_value()) {
      if (EqualsIgnoreCase(*name, "csrf-token") ||
          EqualsIgnoreCase(*name, "_csrf")) {
        return "meta_csrf_token";
      }
      if (EqualsIgnoreCase(*name, "csrf-param") ||
          EqualsIgnoreCase(*name, "_csrf_header")) {
        return "meta_csrf_param";
      }
    }
    return std::nullopt;
  }
  if (IsTagNamed(tag, "<input")) {
    const auto type = FindAttribute(tag, 6, "type");
    if (!type.has_value() || !EqualsIgnoreCase(*type, "hidden")) {
      return std::nullopt;
    }
    const auto name = FindAttribute(tag, 6, "name");
    if (!name.has_value()) return std::nullopt;
    if (EqualsIgnoreCase(*name, "csrfmiddlewaretoken")) {
      return "hidden_csrfmiddlewaretoken";
    }
    if (EqualsIgnoreCase(*name, "_token")) return "hidden_token";
    if (EqualsIgnoreCase(*name, "_wpnonce")) return "hidden_wpnonce";
    if (StartsWithIgnoreCase(*name, "csrf") ||
        StartsWithIgnoreCase(*name, "_csrf")) {
      return "hidden_csrf";
    }
    if (EqualsIgnoreCase(*name, "authenticity_token")) {
      return "hidden_authenticity";
    }
    if (EqualsIgnoreCase(*name, "__RequestVerificationToken")) {
      return "hidden_request_verification";
    }
    return std::nullopt;
  }
  if (IsTagNamed(tag, "<form")) {
    const auto method = FindAttribute(tag, 5, "method");
    if (!method.has_value() || !EqualsIgnoreCase(*method, "post")) {
      return std::nullopt;
    }
    const auto action = FindAttribute(tag, 5, "action");
    if (!action.has_value()) return std::nullopt;
    if (ContainsIgnoreCase(*action, "login") ||
        ContainsIgnoreCase(*action, "signin")) {
      return "post_form_login";
    }
    if (ContainsIgnoreCase(*action, "checkout") ||
        ContainsIgnoreCase(*action, "cart")) {
      return "post_form_checkout";
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// The detail for a sidecar fall-through: the first reason's header name.
std::string SidecarDetail(const SidecarClassification& sidecar) {
  if (sidecar.reasons.empty()) return std::string();
  std::string_view reason = sidecar.reasons.front();
  if (reason == "sidecar-too-large") return "sidecar-too-large";
  const size_t colon = reason.find(':');
  if (colon == std::string_view::npos) return SanitizeDetail(reason);
  const std::string_view kind = reason.substr(0, colon);
  const std::string_view rest = reason.substr(colon + 1);
  if (kind == "vary-unstorable") return "vary";
  return SanitizeDetail(rest);
}

}  // namespace

// ---- the cookie rule -----------------------------------------------------

CookieShareClass CookieShareClassForContentType(std::string_view content_type) {
  std::string_view media = content_type.substr(0, content_type.find(';'));
  media = TrimOws(media);
  if (StartsWithIgnoreCase(media, "image/") && media.size() > 6) {
    return CookieShareClass::kStaticAsset;
  }
  for (std::string_view type : {"text/css", "application/javascript",
                                "text/javascript", "application/x-javascript",
                                "application/ecmascript", "text/ecmascript"}) {
    if (EqualsIgnoreCase(media, type)) return CookieShareClass::kStaticAsset;
  }
  return CookieShareClass::kPage;
}

bool HtmlExplicitlyShared(uint16_t origin_cc_flags, uint32_t origin_s_maxage) {
  if ((origin_cc_flags & Meta::kCCOriginPublic) != 0) return true;
  return (origin_cc_flags & Meta::kCCOriginSMaxagePresent) != 0 &&
         (origin_cc_flags & Meta::kCCOriginSMaxageRepeated) == 0 &&
         origin_s_maxage > 0;
}

bool OriginStatesLifetime(uint16_t origin_cc_flags, uint32_t origin_max_age,
                          uint32_t origin_s_maxage) {
  if ((origin_cc_flags & Meta::kCCOriginHeaderPresent) == 0) return false;
  if (origin_max_age > 0) return true;
  return (origin_cc_flags & Meta::kCCOriginSMaxagePresent) != 0 &&
         origin_s_maxage > 0;
}

bool HtmlCookieRequestMayShare(bool request_has_cookie, bool explicit_shared) {
  return !request_has_cookie || explicit_shared;
}

bool CookieRequestMayStore(bool request_has_cookie,
                           CookieShareClass response_class,
                           bool explicit_shared, bool origin_states_lifetime) {
  if (!request_has_cookie) return true;
  if (response_class == CookieShareClass::kStaticAsset) {
    return origin_states_lifetime;
  }
  return HtmlCookieRequestMayShare(request_has_cookie, explicit_shared);
}

bool CookieRequestMayServe(bool request_has_cookie,
                           CookieShareClass entry_class, bool explicit_shared) {
  if (entry_class == CookieShareClass::kStaticAsset) return true;
  return HtmlCookieRequestMayShare(request_has_cookie, explicit_shared);
}

// ---- tokens --------------------------------------------------------------

bool IsHttpTokenChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
      (u >= 'A' && u <= 'Z')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool IsHttpToken(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!IsHttpTokenChar(c)) return false;
  }
  return true;
}

// ---- names ---------------------------------------------------------------

std::string_view HtmlStoreActionName(HtmlStoreAction action) {
  switch (action) {
    case HtmlStoreAction::kRefuse:
      return "refuse";
    case HtmlStoreAction::kStore:
      return "store";
    case HtmlStoreAction::kStoreNoNotify:
      return "store_no_notify";
    case HtmlStoreAction::kWouldStore:
      return "would_store";
    case HtmlStoreAction::kWouldStoreNoNotify:
      return "would_store_no_notify";
  }
  return "unknown";
}

std::string_view HtmlStoreReasonName(HtmlStoreReason reason) {
  static constexpr std::array<std::string_view, kHtmlStoreReasonCount> kNames =
      {"none",
       "method",
       "status",
       "incomplete",
       "auth_request",
       "cookie_request",
       "set_cookie",
       "private",
       "no_store",
       "no_lifetime",
       "vary",
       "headers",
       "nonce_csp",
       "nonce_link",
       "content_type",
       "encoding",
       "size",
       "url",
       "personalisation_signal",
       "no_transform",
       "mode"};
  const auto index = static_cast<size_t>(reason);
  return index < kNames.size() ? kNames[index] : "unknown";
}

std::string_view HtmlServeActionName(HtmlServeAction action) {
  switch (action) {
    case HtmlServeAction::kFallThrough:
      return "fall_through";
    case HtmlServeAction::kServeHtml:
      return "serve_html";
    case HtmlServeAction::kServeMarkdown:
      return "serve_markdown";
    case HtmlServeAction::kValidate:
      return "validate";
  }
  return "unknown";
}

std::string_view HtmlServeReasonName(HtmlServeReason reason) {
  static constexpr std::array<std::string_view, kHtmlServeReasonCount> kNames =
      {"fresh",
       "mode",
       "auth_request",
       "cookie_request",
       "client_no_cache",
       "needs_validation",
       "stale",
       "headers_not_reproducible",
       "sidecar_missing",
       "purged",
       "absent",
       "binding_stale",
       "parent_stale",
       "method",
       "range",
       "cors",
       "no_transform",
       "withheld",
       "skew"};
  const auto index = static_cast<size_t>(reason);
  return index < kNames.size() ? kNames[index] : "unknown";
}

std::span<const std::string_view> HtmlPersonalisationSignalNames() {
  return kSignalNames;
}

std::string_view HtmlCacheModeName(HtmlCacheMode mode) {
  switch (mode) {
    case HtmlCacheMode::kOff:
      return "off";
    case HtmlCacheMode::kReportOnly:
      return "report-only";
    case HtmlCacheMode::kServeVariants:
      return "serve-variants";
    case HtmlCacheMode::kServeCachedHtml:
      return "serve-cached-html";
  }
  return "unknown";
}

std::string HtmlStoreVerdict::reason_code() const {
  std::string code(HtmlStoreReasonName(reason));
  if ((reason == HtmlStoreReason::kHeaders ||
       reason == HtmlStoreReason::kVary) &&
      !detail.empty()) {
    code.push_back(':');
    code.append(detail);
  }
  return code;
}

std::string HtmlServeVerdict::reason_code() const {
  return std::string(HtmlServeReasonName(reason));
}

// ---- date ----------------------------------------------------------------

namespace {

constexpr std::string_view kShortDays[] = {"Sun", "Mon", "Tue", "Wed",
                                           "Thu", "Fri", "Sat"};
constexpr std::string_view kLongDays[] = {"Sunday",    "Monday",   "Tuesday",
                                          "Wednesday", "Thursday", "Friday",
                                          "Saturday"};
constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                        "May", "Jun", "Jul", "Aug",
                                        "Sep", "Oct", "Nov", "Dec"};

// Index of `s` in `names`, or -1.  Case-sensitive: these are fixed tokens.
int IndexIn(std::string_view s, std::span<const std::string_view> names) {
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == s) return static_cast<int>(i);
  }
  return -1;
}

// Year of the civil date `days` after 1970-01-01 (proleptic Gregorian).
int64_t YearFromDays(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  return static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
}

// RFC 9110 section 5.6.7: a two-digit year is read as the year with those
// last two digits that lies within 50 years of the receipt time; one that
// would be more than 50 years in the future is the most recent past year
// with the same digits.
int64_t ResolveTwoDigitYear(unsigned yy, uint32_t now_seconds) {
  const int64_t now_year =
      YearFromDays(static_cast<int64_t>(now_seconds) / 86400);
  int64_t year = now_year - (now_year % 100) + static_cast<int64_t>(yy);
  if (year > now_year + 50) {
    year -= 100;
  } else if (year + 100 <= now_year + 50) {
    year += 100;
  }
  return year;
}

}  // namespace

std::optional<int64_t> ParseHttpDateSeconds(std::string_view value,
                                            uint32_t now_seconds) {
  // Optional whitespace around a field value is not part of it (RFC 9110
  // section 5.5).  Each form is then a fixed shape.
  value = TrimOws(value);
  int weekday = -1;
  std::string_view day_text;
  std::string_view month_text;
  std::string_view time_text;
  int64_t year = 0;
  unsigned four_digit_year = 0;
  if (value.size() == 29 && value[3] == ',') {
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
    if (value[4] != ' ' || value[7] != ' ' || value[11] != ' ' ||
        value[16] != ' ' || value[25] != ' ' || value.substr(26) != "GMT") {
      return std::nullopt;
    }
    weekday = IndexIn(value.substr(0, 3), kShortDays);
    day_text = value.substr(5, 2);
    month_text = value.substr(8, 3);
    if (!ParseDigits(value.substr(12, 4), &four_digit_year)) {
      return std::nullopt;
    }
    year = four_digit_year;
    time_text = value.substr(17, 8);
  } else if (value.size() == 24 && value[3] == ' ') {
    // asctime-date: "Sun Nov  6 08:49:37 1994"; a day below 10 is padded
    // with a space (2DIGIT / (SP 1DIGIT)).
    if (value[7] != ' ' || value[10] != ' ' || value[19] != ' ') {
      return std::nullopt;
    }
    weekday = IndexIn(value.substr(0, 3), kShortDays);
    month_text = value.substr(4, 3);
    day_text = value.substr(8, 2);
    if (day_text.front() == ' ') day_text.remove_prefix(1);
    time_text = value.substr(11, 8);
    if (!ParseDigits(value.substr(20, 4), &four_digit_year)) {
      return std::nullopt;
    }
    year = four_digit_year;
  } else {
    // rfc850-date: "Sunday, 06-Nov-94 08:49:37 GMT".
    const size_t comma = value.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    weekday = IndexIn(value.substr(0, comma), kLongDays);
    const std::string_view rest = value.substr(comma + 1);
    if (rest.size() != 23 || rest[0] != ' ' || rest[3] != '-' ||
        rest[7] != '-' || rest[10] != ' ' || rest[19] != ' ' ||
        rest.substr(20) != "GMT") {
      return std::nullopt;
    }
    day_text = rest.substr(1, 2);
    month_text = rest.substr(4, 3);
    unsigned two_digit_year = 0;
    if (!ParseDigits(rest.substr(8, 2), &two_digit_year)) return std::nullopt;
    year = ResolveTwoDigitYear(two_digit_year, now_seconds);
    time_text = rest.substr(11, 8);
  }
  if (weekday < 0) return std::nullopt;
  const int month_index = IndexIn(month_text, kMonths);
  if (month_index < 0) return std::nullopt;
  const auto month = static_cast<unsigned>(month_index) + 1;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (day_text.empty() || time_text[2] != ':' || time_text[5] != ':' ||
      !ParseDigits(day_text, &day) ||
      !ParseDigits(time_text.substr(0, 2), &hour) ||
      !ParseDigits(time_text.substr(3, 2), &minute) ||
      !ParseDigits(time_text.substr(6, 2), &second)) {
    return std::nullopt;
  }
  if (day < 1 || day > DaysInMonth(year, month)) return std::nullopt;
  if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
  const int64_t days = DaysFromCivil(year, month, day);
  // 1970-01-01 was a Thursday (weekday index 4 with Sunday = 0).
  const int64_t expected = ((days % 7) + 7 + 4) % 7;
  if (expected != weekday) return std::nullopt;
  return days * 86400 + static_cast<int64_t>(hour) * 3600 +
         static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
}

// ---- personalisation -----------------------------------------------------

namespace {

// `body_continues`: the body goes on past the scan window, so a tag still open
// at the window's edge has attributes nobody read.
std::optional<std::string_view> ScanForSignal(std::string_view body_prefix,
                                              bool body_continues) {
  if (body_prefix.size() > kPersonalisationScanBytes) {
    body_prefix = body_prefix.substr(0, kPersonalisationScanBytes);
    body_continues = true;
  }
  size_t pos = 0;
  while (pos < body_prefix.size()) {
    pos = body_prefix.find('<', pos);
    if (pos == std::string_view::npos) break;
    const std::string_view rest = body_prefix.substr(pos);
    if (!IsTagNamed(rest, "<meta") && !IsTagNamed(rest, "<input") &&
        !IsTagNamed(rest, "<form")) {
      ++pos;
      continue;
    }
    bool unclosed = false;
    const std::string_view tag = TagText(body_prefix, pos, &unclosed);
    if (unclosed && (tag.size() == kMaxTagBytes || body_continues)) {
      return "oversized_tag";
    }
    if (auto signal = SignalForTag(tag)) return signal;
    // The tag's text is attribute text, not markup: a '<' inside it opens
    // nothing.  Resuming after it also keeps the scan linear.
    pos += tag.size();
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string_view> FindPersonalisationSignal(
    std::string_view body_prefix) {
  return ScanForSignal(body_prefix, /*body_continues=*/false);
}

// ---- store ---------------------------------------------------------------

namespace {

// The store rules, independent of the mode.
HtmlStoreVerdict ApplyStoreRules(const HtmlStoreRequest& request,
                                 const HtmlStoreResponse& response,
                                 const HtmlStorePolicy& policy) {
  HtmlStoreVerdict v;
  const std::span<const HeaderField> headers = response.headers;

  // 1. Method, status, completeness.
  if (request.method != "GET")
    return Refuse(std::move(v), HtmlStoreReason::kMethod);
  if (response.status != 200)
    return Refuse(std::move(v), HtmlStoreReason::kStatus);
  if (!response.body_complete ||
      FindHeader(headers, "content-range") != nullptr) {
    return Refuse(std::move(v), HtmlStoreReason::kIncomplete);
  }

  // 2. Key-safe path, and not a URL the optimizer itself produced.  The
  // marker is matched anywhere in the path (it sits mid-name in a rewritten
  // filename); refusing a path that merely looks rewritten costs one entry.
  if (request.url_path.empty() || request.url_path.front() != '/' ||
      request.url_path.find_first_of("?%#;") != std::string_view::npos ||
      request.url_path.find(kRewrittenUrlMarker) != std::string_view::npos) {
    return Refuse(std::move(v), HtmlStoreReason::kUrl);
  }

  // 3. Content type and encoding.
  if (CountHeader(headers, "content-type") != 1) {
    return Refuse(std::move(v), HtmlStoreReason::kContentType);
  }
  {
    std::string_view type = FindHeader(headers, "content-type")->value;
    type = TrimOws(type.substr(0, type.find(';')));
    if (!EqualsIgnoreCase(type, "text/html")) {
      return Refuse(std::move(v), HtmlStoreReason::kContentType);
    }
  }
  for (const HeaderField& h : headers) {
    if (EqualsIgnoreCase(h.name, "content-encoding") &&
        !EqualsIgnoreCase(TrimOws(h.value), "identity")) {
      return Refuse(std::move(v), HtmlStoreReason::kEncoding);
    }
  }

  // 4. No credentials.
  if (request.has_authorization || request.authenticated_user) {
    return Refuse(std::move(v), HtmlStoreReason::kAuthRequest);
  }

  // 5. Request no-store (RFC 9111 section 5.2.1.5).
  if (request.request_no_store) {
    return Refuse(std::move(v), HtmlStoreReason::kNoStore, "request");
  }

  // 6. No Set-Cookie.
  if (FindHeader(headers, "set-cookie") != nullptr ||
      FindHeader(headers, "set-cookie2") != nullptr) {
    return Refuse(std::move(v), HtmlStoreReason::kSetCookie);
  }

  // 7. Cache-Control.
  OriginCacheControl cc;
  size_t max_age_count = 0;
  size_t s_maxage_count = 0;
  for (const HeaderField& h : headers) {
    if (!EqualsIgnoreCase(h.name, "cache-control")) continue;
    AccumulateOriginCacheControl(h.value, &cc);
    max_age_count += CountDirective(h.value, "max-age");
    s_maxage_count += CountDirective(h.value, "s-maxage");
  }
  v.cc_flags = cc.cc_flags;
  v.cc_evaluated = true;
  v.explicit_shared = HtmlExplicitlyShared(cc.cc_flags, cc.s_maxage);
  if ((cc.cc_flags & Meta::kCCOriginPrivate) != 0) {
    return Refuse(std::move(v), HtmlStoreReason::kPrivate);
  }
  if ((cc.cc_flags & Meta::kCCOriginNoStore) != 0) {
    return Refuse(std::move(v), HtmlStoreReason::kNoStore);
  }

  // 8. Cookie request.
  if (!HtmlCookieRequestMayShare(request.has_cookie, v.explicit_shared)) {
    return Refuse(std::move(v), HtmlStoreReason::kCookieRequest);
  }

  // 9. Vary: only Accept-Encoding is admitted.
  {
    std::string joined;
    for (const HeaderField& h : headers) {
      if (!EqualsIgnoreCase(h.name, "vary")) continue;
      if (!joined.empty()) joined.append(", ");
      joined.append(h.value);
    }
    std::string_view rest = joined;
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      const std::string_view token = TrimOws(rest.substr(0, comma));
      rest = comma == std::string_view::npos ? std::string_view()
                                             : rest.substr(comma + 1);
      if (!token.empty()) v.vary_tokens.push_back(Lower(token));
    }
    for (const std::string& token : v.vary_tokens) {
      if (token != "accept-encoding") {
        std::string detail = SanitizeDetail(token);
        return Refuse(std::move(v), HtmlStoreReason::kVary, std::move(detail));
      }
    }
  }

  // 10. Header reproducibility.
  v.sidecar = ClassifyForHeadersSidecar(headers);
  v.sidecar_evaluated = true;
  if (v.sidecar.verdict == SidecarVerdict::kNeverOptimized) {
    const auto& reasons = v.sidecar.reasons;
    const bool csp =
        std::find(reasons.begin(), reasons.end(), "nonce-csp") != reasons.end();
    return Refuse(std::move(v), csp ? HtmlStoreReason::kNonceCsp
                                    : HtmlStoreReason::kNonceLink);
  }
  if (v.sidecar.verdict == SidecarVerdict::kFallThrough) {
    std::string detail = SidecarDetail(v.sidecar);
    return Refuse(std::move(v), HtmlStoreReason::kHeaders, std::move(detail));
  }

  // 11. Explicit freshness, never heuristic.
  {
    const HeaderField* expires = FindHeader(headers, "expires");
    // Repeated Expires lines cannot be told apart, so none of them is
    // trusted: an invalid value means already expired (RFC 9111 section 5.3).
    const bool single_expires = CountHeader(headers, "expires") == 1;
    const bool no_cache = (cc.cc_flags & Meta::kCCOriginNoCache) != 0;
    if ((cc.cc_flags & Meta::kCCOriginSMaxagePresent) != 0) {
      v.lifetime_s = cc.s_maxage;
      v.lifetime_explicit = true;
    } else if (max_age_count > 0) {
      v.lifetime_s = cc.max_age;
      v.lifetime_explicit = true;
    } else if (expires != nullptr) {
      v.lifetime_explicit = true;
      int64_t date = policy.now_seconds;
      // As with Expires, repeated lines cannot be told apart, so the receipt
      // time stands in.
      if (CountHeader(headers, "date") == 1) {
        if (auto parsed = ParseHttpDateSeconds(
                FindHeader(headers, "date")->value, policy.now_seconds)) {
          date = *parsed;
        }
      }
      if (auto parsed = single_expires ? ParseHttpDateSeconds(
                                             expires->value, policy.now_seconds)
                                       : std::nullopt) {
        v.lifetime_s = SaturateU32(*parsed - date);
      }
    }
    // Conflicting lifetimes: stale, so validated before every use.
    const bool conflicting = max_age_count > 1 || s_maxage_count > 1;
    if (conflicting) v.lifetime_s = 0;
    v.needs_validation = v.lifetime_s == 0 || no_cache || conflicting;
    for (const HeaderField& h : headers) {
      if ((EqualsIgnoreCase(h.name, "etag") ||
           EqualsIgnoreCase(h.name, "last-modified")) &&
          !TrimOws(h.value).empty()) {
        v.has_validators = true;
      }
    }
    if ((!v.lifetime_explicit && !v.has_validators) ||
        (v.needs_validation && !v.has_validators)) {
      return Refuse(std::move(v), HtmlStoreReason::kNoLifetime);
    }
  }

  // 12. Size.
  if (response.body_bytes >
      std::min(policy.max_body_bytes, kHtmlStoreHardMaxBodyBytes)) {
    return Refuse(std::move(v), HtmlStoreReason::kSize);
  }

  // 13. Personalisation signals.  A prefix shorter than the scan window would
  // pass the scan unread, so it refuses rather than stores.
  if (response.body_prefix.size() <
      std::min<uint64_t>(response.body_bytes, kPersonalisationScanBytes)) {
    return Refuse(std::move(v), HtmlStoreReason::kIncomplete, "body_prefix");
  }
  if (auto signal =
          ScanForSignal(response.body_prefix,
                        response.body_bytes > kPersonalisationScanBytes)) {
    return Refuse(std::move(v), HtmlStoreReason::kPersonalisationSignal,
                  std::string(*signal));
  }

  // 14. no-transform.
  if ((cc.cc_flags & Meta::kCCOriginNoTransform) != 0) {
    v.action = HtmlStoreAction::kStoreNoNotify;
    v.reason = HtmlStoreReason::kNoTransform;
  } else {
    v.action = HtmlStoreAction::kStore;
    v.reason = HtmlStoreReason::kNone;
  }
  return v;
}

}  // namespace

HtmlStoreVerdict ClassifyHtmlForStore(const HtmlStoreRequest& request,
                                      const HtmlStoreResponse& response,
                                      const HtmlStorePolicy& policy) {
  if (policy.mode == HtmlCacheMode::kOff) {
    return Refuse(HtmlStoreVerdict{}, HtmlStoreReason::kMode);
  }
  HtmlStoreVerdict v = ApplyStoreRules(request, response, policy);
  if (policy.mode != HtmlCacheMode::kReportOnly) return v;
  // Report-only has no action that writes.  A personalisation match stays a
  // refusal: its reason and detail measure it, and an action that invites
  // storing or notifying would be wrong for that page.
  if (v.action == HtmlStoreAction::kStore) {
    v.action = HtmlStoreAction::kWouldStore;
  } else if (v.action == HtmlStoreAction::kStoreNoNotify) {
    v.action = HtmlStoreAction::kWouldStoreNoNotify;
  }
  return v;
}

// ---- serve ---------------------------------------------------------------

HtmlServeVerdict ClassifyHtmlForServe(const HtmlServeRequest& request,
                                      const StoredHtmlFacts& stored) {
  HtmlServeVerdict v;
  v.enforce = request.mode == HtmlCacheMode::kServeVariants ||
              request.mode == HtmlCacheMode::kServeCachedHtml;
  auto done = [&v](HtmlServeAction action, HtmlServeReason reason) {
    v.action = action;
    v.reason = reason;
    return v;
  };
  constexpr HtmlServeAction kFall = HtmlServeAction::kFallThrough;

  // 1. Mode.  Nothing is stored in report-only, so it has no serve verdict.
  if (request.mode == HtmlCacheMode::kOff ||
      request.mode == HtmlCacheMode::kReportOnly) {
    return done(kFall, HtmlServeReason::kMode);
  }
  const bool cached_html = request.mode == HtmlCacheMode::kServeCachedHtml;

  // 1a. Request shape.
  if (!request.method_cacheable) return done(kFall, HtmlServeReason::kMethod);
  if (request.has_range) return done(kFall, HtmlServeReason::kRange);
  if (request.has_origin) return done(kFall, HtmlServeReason::kCors);

  // 2. Absent entry.
  if (!stored.present) return done(kFall, HtmlServeReason::kAbsent);

  // 2a. Skew.
  if (static_cast<int64_t>(stored.stored_at) >
      static_cast<int64_t>(request.now_seconds) + kHtmlServeSkewSeconds) {
    return done(kFall, HtmlServeReason::kSkew);
  }

  const int64_t elapsed =
      std::max<int64_t>(0, static_cast<int64_t>(request.now_seconds) -
                               static_cast<int64_t>(stored.stored_at));
  v.age_s = SaturateU32(static_cast<int64_t>(stored.age_at_store) + elapsed);
  const int64_t age = v.age_s;

  // 3. Credentials.
  if (request.has_authorization || request.authenticated_user) {
    return done(kFall, HtmlServeReason::kAuthRequest);
  }
  // 4. Cookie request.
  if (!HtmlCookieRequestMayShare(request.has_cookie, stored.explicit_shared)) {
    return done(kFall, HtmlServeReason::kCookieRequest);
  }
  // 5. Client no-cache.
  if (request.cache_control_no_cache ||
      (request.pragma_no_cache && !request.has_cache_control)) {
    return done(kFall, HtmlServeReason::kClientNoCache);
  }
  // 6. Purged.
  if (stored.purged) return done(kFall, HtmlServeReason::kPurged);
  // 7. Header replay.
  if (stored.headers_not_reproducible) {
    return done(kFall, HtmlServeReason::kHeadersNotReproducible);
  }
  if (stored.sidecar_required && !stored.sidecar_present) {
    return done(kFall, HtmlServeReason::kSidecarMissing);
  }

  // 8. Freshness.  Stale entries are never served.
  const bool must_validate = stored.needs_validation || stored.lifetime_s == 0;
  const int64_t lifetime = static_cast<int64_t>(stored.lifetime_s);
  bool fresh = !must_validate && age < lifetime;
  if (request.max_age >= 0 && age > request.max_age) fresh = false;
  if (request.min_fresh >= 0) {
    // RFC 9111 section 5.2.1.3: the entry must still be fresh min_fresh
    // seconds from now, i.e. age + min_fresh <= lifetime, written without a
    // sum that can overflow.
    if (request.min_fresh > lifetime || age > lifetime - request.min_fresh) {
      fresh = false;
    }
  }
  // A 304 the origin answered just now counts only in kServeCachedHtml, which
  // has a validation step; kServeVariants has none, so the flag is not
  // consulted there.
  const bool validated = cached_html && stored.validated_this_request;
  if (!fresh && !validated) {
    const HtmlServeAction action =
        cached_html ? HtmlServeAction::kValidate : kFall;
    if (request.wants_markdown) {
      return done(action, HtmlServeReason::kParentStale);
    }
    return done(action, must_validate ? HtmlServeReason::kNeedsValidation
                                      : HtmlServeReason::kStale);
  }

  // 9. Markdown.  The copy is served only while age < min(lifetime,
  // render_ttl_s).  An entry the origin confirmed this request drops the
  // parent-lifetime term (the parent was just confirmed) and keeps the
  // render bound, so a copy older than its own lifetime is still refused.
  if (request.wants_markdown) {
    if (stored.no_transform || request.request_no_transform) {
      return done(kFall, HtmlServeReason::kNoTransform);
    }
    if (stored.withheld) return done(kFall, HtmlServeReason::kWithheld);
    if (!stored.variant_present) return done(kFall, HtmlServeReason::kAbsent);
    if (!stored.binding_live)
      return done(kFall, HtmlServeReason::kBindingStale);
    const int64_t bound =
        validated ? static_cast<int64_t>(stored.render_ttl_s)
                  : std::min<int64_t>(lifetime, stored.render_ttl_s);
    if (age >= bound) return done(kFall, HtmlServeReason::kParentStale);
    return done(HtmlServeAction::kServeMarkdown, HtmlServeReason::kFresh);
  }

  // 10. HTML.
  if (cached_html) {
    return done(HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  }
  return done(kFall, HtmlServeReason::kMode);
}

namespace {

// RFC 9111 section 1.2.2 delta-seconds, saturating at INT32_MAX (2^31, the
// section's own ceiling, minus one so it fits the signed field); -1 when the
// value is not delta-seconds at all.  A quoted value is accepted, as section
// 5.2 asks of a recipient.
int64_t ParseDeltaSeconds(std::string_view v) {
  v = TrimOws(v);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    v = v.substr(1, v.size() - 2);
  }
  if (v.empty()) return -1;
  int64_t out = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return -1;
    out = std::min<int64_t>(out * 10 + (c - '0'), INT32_MAX);
  }
  return out;
}

}  // namespace

void ApplyHtmlRequestCacheControl(std::string_view cache_control,
                                  HtmlServeRequest* request) {
  request->has_cache_control = true;
  while (true) {
    const size_t comma = cache_control.find(',');
    const std::string_view item = TrimOws(cache_control.substr(0, comma));
    const size_t eq = item.find('=');
    const std::string_view name = TrimOws(item.substr(0, eq));
    const std::string_view arg =
        eq == std::string_view::npos ? std::string_view() : item.substr(eq + 1);
    if (EqualsIgnoreCase(name, "no-cache")) {
      request->cache_control_no_cache = true;
    } else if (EqualsIgnoreCase(name, "no-transform")) {
      request->request_no_transform = true;
    } else if (EqualsIgnoreCase(name, "max-age") ||
               EqualsIgnoreCase(name, "min-fresh")) {
      const int64_t seconds = ParseDeltaSeconds(arg);
      if (seconds < 0) {
        // Not delta-seconds: read as max-age=0, the strictest value, so only
        // an entry of age 0 can still answer.
        request->max_age = 0;
      } else if (EqualsIgnoreCase(name, "max-age")) {
        request->max_age = request->max_age < 0
                               ? seconds
                               : std::min(request->max_age, seconds);
      } else {
        request->min_fresh = std::max(request->min_fresh, seconds);
      }
    }
    if (comma == std::string_view::npos) return;
    cache_control = cache_control.substr(comma + 1);
  }
}

}  // namespace pagespeed

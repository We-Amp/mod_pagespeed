// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/capability_mask.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "lib/base/string_util.h"

namespace pagespeed {

namespace {

// Case-insensitive substring search operating directly on string_view.
// Zero allocations — works character-by-character with inline LowerChar.
size_t FindCaseInsensitive(std::string_view haystack, std::string_view needle,
                           size_t pos = 0) {
  if (needle.empty())
    return pos <= haystack.size() ? pos : std::string_view::npos;
  if (needle.size() > haystack.size()) return std::string_view::npos;
  size_t limit = haystack.size() - needle.size();
  for (size_t i = pos; i <= limit; ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      auto a = static_cast<unsigned char>(haystack[i + j]);
      auto b = static_cast<unsigned char>(needle[j]);
      if (net_instaweb::LowerChar(a) != net_instaweb::LowerChar(b)) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return std::string_view::npos;
}

// Check if Accept header contains a specific MIME type as a complete token.
// Handles comma-separated types with optional quality values, e.g.:
//   "image/avif, image/webp;q=0.8, */*"
// MIME types are ASCII lowercase, so exact match is fine (browsers send
// lowercase MIME types in Accept).
bool AcceptContains(std::string_view accept, std::string_view mime_type) {
  size_t pos = 0;
  while (pos < accept.size()) {
    size_t found = accept.find(mime_type, pos);
    if (found == std::string_view::npos) {
      return false;
    }
    // Check left boundary: must be start-of-string, comma, or whitespace.
    bool left_ok = (found == 0) || accept[found - 1] == ',' ||
                   accept[found - 1] == ' ' || accept[found - 1] == '\t';
    // Check right boundary: must be end-of-string, comma, semicolon, or
    // whitespace (quality params follow ';').
    size_t end = found + mime_type.size();
    bool right_ok = (end >= accept.size()) || accept[end] == ',' ||
                    accept[end] == ';' || accept[end] == ' ' ||
                    accept[end] == '\t';
    if (left_ok && right_ok) {
      return true;
    }
    pos = found + 1;
  }
  return false;
}

// Parse viewport from User-Agent header.
// Zero allocations — uses case-insensitive search on string_view directly.
CapabilityMask::Viewport ParseViewport(std::string_view user_agent) {
  bool has_mobile =
      FindCaseInsensitive(user_agent, "mobile") != std::string_view::npos;
  bool has_android =
      FindCaseInsensitive(user_agent, "android") != std::string_view::npos;
  bool has_ipad =
      FindCaseInsensitive(user_agent, "ipad") != std::string_view::npos;

  // Tablet indicators — check BEFORE mobile because iPad UA strings
  // contain both "iPad" and "Mobile" (e.g., "iPad; ... Mobile/15E148").
  if (has_ipad ||
      FindCaseInsensitive(user_agent, "tablet") != std::string_view::npos) {
    return CapabilityMask::Viewport::kTablet;
  }

  // Android tablet: has "Android" but not "Mobile" in UA.
  if (has_android && !has_mobile) {
    return CapabilityMask::Viewport::kTablet;
  }

  // Mobile indicators
  if (has_mobile || has_android ||
      FindCaseInsensitive(user_agent, "iphone") != std::string_view::npos ||
      FindCaseInsensitive(user_agent, "ipod") != std::string_view::npos) {
    return CapabilityMask::Viewport::kMobile;
  }

  // Default to desktop
  return CapabilityMask::Viewport::kDesktop;
}

// Case-insensitive comparison of two string_views.
bool EqualsCI(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (net_instaweb::LowerChar(a[i]) != net_instaweb::LowerChar(b[i])) {
      return false;
    }
  }
  return true;
}

// Check if a token appears in an Accept-Encoding header value, respecting
// q=0 exclusions.  Handles comma-separated tokens with optional quality
// values (e.g. "gzip, br;q=1.0, identity;q=0").
// Zero allocations — operates directly on string_view.
bool EncodingAccepted(std::string_view accept_encoding,
                      std::string_view token) {
  size_t pos = 0;
  while (pos < accept_encoding.size()) {
    // Skip leading whitespace.
    while (pos < accept_encoding.size() &&
           (accept_encoding[pos] == ' ' || accept_encoding[pos] == '\t'))
      ++pos;

    // Find end of this token (comma or end).
    size_t comma = accept_encoding.find(',', pos);
    if (comma == std::string_view::npos) comma = accept_encoding.size();

    std::string_view entry = accept_encoding.substr(pos, comma - pos);

    // Split on ';' to separate token from parameters.
    size_t semi = entry.find(';');
    std::string_view name = entry.substr(0, semi);
    // Trim trailing whitespace from name.
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
      name = name.substr(0, name.size() - 1);
    }

    if (EqualsCI(name, token)) {
      // Check for q=0 (explicitly disabled).
      if (semi != std::string_view::npos) {
        std::string_view params = entry.substr(semi + 1);
        // Look for q= parameter.
        size_t q_pos = FindCaseInsensitive(params, "q=");
        if (q_pos != std::string_view::npos) {
          std::string_view q_val = params.substr(q_pos + 2);
          // Trim whitespace.
          while (!q_val.empty() &&
                 (q_val.front() == ' ' || q_val.front() == '\t')) {
            q_val = q_val.substr(1);
          }
          if (q_val.starts_with("0") && (q_val.size() == 1 || q_val[1] == '.' ||
                                         q_val[1] == ',' || q_val[1] == ' ')) {
            // Check if it's actually 0 (not 0.5 etc).
            // "0", "0.0", "0.00", "0.000" all mean disabled.
            bool is_zero = true;
            for (char ch : q_val) {
              if (ch == ',' || ch == ' ') break;
              if (ch != '0' && ch != '.') {
                is_zero = false;
                break;
              }
            }
            if (is_zero) return false;
          }
        }
      }
      return true;
    }

    pos = comma + 1;
  }
  return false;
}

// Parse a Sec-CH-DPR value ("1", "1.5", "2", "3.0", ...) and report
// whether it indicates a high-density (>= 1.5) display.  Returns
// nullopt for absent or unparseable values so callers fall back to
// UA heuristics.  Zero allocations.
std::optional<bool> DprHintIsHighDensity(std::string_view value) {
  // Trim optional whitespace.
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  if (value.empty()) return std::nullopt;

  // <digits> [ '.' <digits> ] — anything else is unparseable.
  size_t i = 0;
  uint32_t int_part = 0;
  bool has_int = false;
  while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
    if (int_part < 1000) int_part = int_part * 10 + (value[i] - '0');
    has_int = true;
    ++i;
  }
  uint32_t first_frac = 0;
  bool has_frac = false;
  if (i < value.size() && value[i] == '.') {
    ++i;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
      if (!has_frac) first_frac = value[i] - '0';
      has_frac = true;
      ++i;
    }
  }
  if (i != value.size() || (!has_int && !has_frac)) return std::nullopt;
  return int_part >= 2 || (int_part == 1 && first_frac >= 5);
}

// Parse Accept-Encoding header to determine transfer encoding.
// Priority: br > gzip > identity.
// Handles q=0 exclusions and the wildcard '*'.
// Zero allocations — all operations on string_view.
CapabilityMask::TransferEncoding ParseAcceptEncoding(
    std::string_view accept_encoding) {
  if (accept_encoding.empty()) {
    return CapabilityMask::TransferEncoding::kIdentity;
  }

  bool has_br = EncodingAccepted(accept_encoding, "br");
  bool has_gzip = EncodingAccepted(accept_encoding, "gzip");
  bool has_star = EncodingAccepted(accept_encoding, "*");

  if (has_br) {
    return CapabilityMask::TransferEncoding::kBrotli;
  }
  if (has_star) {
    // '*' means accept anything, but explicit q=0 entries take precedence.
    // EncodingAccepted("br") returned false: br was either absent or q=0.
    // If br was not mentioned at all, wildcard covers it.
    if (FindCaseInsensitive(accept_encoding, "br") == std::string_view::npos) {
      return CapabilityMask::TransferEncoding::kBrotli;
    }
    // br was explicitly disabled. Try gzip.
    if (has_gzip) {
      return CapabilityMask::TransferEncoding::kGzip;
    }
    if (FindCaseInsensitive(accept_encoding, "gzip") ==
        std::string_view::npos) {
      return CapabilityMask::TransferEncoding::kGzip;
    }
    return CapabilityMask::TransferEncoding::kIdentity;
  }
  if (has_gzip) {
    return CapabilityMask::TransferEncoding::kGzip;
  }
  return CapabilityMask::TransferEncoding::kIdentity;
}

}  // namespace

CapabilityMask::CapabilityMask()
    : image_format_(ImageFormat::kOriginal),
      viewport_(Viewport::kDesktop),
      pixel_density_(PixelDensity::k1x),
      save_data_(SaveData::kOff),
      transfer_encoding_(TransferEncoding::kIdentity) {}

CapabilityMask::CapabilityMask(ImageFormat image_format, Viewport viewport,
                               PixelDensity density, SaveData save_data,
                               TransferEncoding encoding)
    : image_format_(image_format),
      viewport_(viewport),
      pixel_density_(density),
      save_data_(save_data),
      transfer_encoding_(encoding) {}

namespace {

// RFC 9110 section 5.6.2 tchar.
bool IsTchar(unsigned char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z')) {
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

void SkipOws(std::string_view s, size_t* pos) {
  while (*pos < s.size() && (s[*pos] == ' ' || s[*pos] == '\t')) ++*pos;
}

std::string_view ParseToken(std::string_view s, size_t* pos) {
  const size_t start = *pos;
  while (*pos < s.size() && IsTchar(static_cast<unsigned char>(s[*pos]))) {
    ++*pos;
  }
  return s.substr(start, *pos - start);
}

// RFC 9110 section 5.6.4 quoted-string; *pos is at the opening quote.
bool SkipQuotedString(std::string_view s, size_t* pos) {
  ++*pos;
  while (*pos < s.size()) {
    auto c = static_cast<unsigned char>(s[*pos]);
    if (c == '"') {
      ++*pos;
      return true;
    }
    if (c == '\\') {
      if (*pos + 1 >= s.size()) return false;
      c = static_cast<unsigned char>(s[*pos + 1]);
      ++*pos;
    }
    // qdtext and quoted-pair: HTAB, SP, VCHAR, obs-text.
    if (c != '\t' && (c < 0x20 || c == 0x7f)) return false;
    ++*pos;
  }
  return false;
}

// RFC 9110 section 12.4.2:
//   qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] )
// Stores the value in thousandths.
bool ParseQValue(std::string_view v, int* q) {
  if (v.empty() || (v[0] != '0' && v[0] != '1')) return false;
  const int whole = v[0] - '0';
  int frac = 0;
  if (v.size() > 1) {
    if (v[1] != '.' || v.size() > 5) return false;
    for (size_t i = 2; i < 5; ++i) {
      int digit = 0;
      if (i < v.size()) {
        if (v[i] < '0' || v[i] > '9') return false;
        digit = v[i] - '0';
      }
      frac = frac * 10 + digit;
    }
  }
  if (whole == 1 && frac != 0) return false;
  *q = whole * 1000 + frac;
  return true;
}

void KeepMax(int q, int* slot) {
  if (q > *slot) *slot = q;
}

}  // namespace

// One media range with its parameters, starting at *pos. Returns false when it
// is malformed. RFC 9110 sections 5.6.6 and 12.5.1.
bool AcceptMarkdownPreference::ParseElement(std::string_view s, size_t* pos) {
  const std::string_view type = ParseToken(s, pos);
  if (type.empty() || *pos >= s.size() || s[*pos] != '/') return false;
  ++*pos;
  const std::string_view subtype = ParseToken(s, pos);
  if (subtype.empty()) return false;
  if (type == "*" && subtype != "*") return false;

  int q = 1000;
  bool have_q = false;
  for (;;) {
    const size_t before = *pos;
    SkipOws(s, pos);
    if (*pos >= s.size() || s[*pos] != ';') {
      *pos = before;
      break;
    }
    ++*pos;
    SkipOws(s, pos);
    // An empty parameter is allowed: parameters = *( OWS ";" OWS [ parameter ] )
    if (*pos >= s.size() || s[*pos] == ';' || s[*pos] == ',') continue;
    const std::string_view name = ParseToken(s, pos);
    if (name.empty() || *pos >= s.size() || s[*pos] != '=') return false;
    ++*pos;
    const bool is_q = EqualsCI(name, "q");
    if (*pos < s.size() && s[*pos] == '"') {
      // The weight is never quoted.
      if (is_q || !SkipQuotedString(s, pos)) return false;
      continue;
    }
    const std::string_view value = ParseToken(s, pos);
    if (value.empty()) return false;
    if (is_q) {
      if (have_q || !ParseQValue(value, &q)) return false;
      have_q = true;
    }
  }

  if (type == "*") {
    KeepMax(q, &any_q_);
  } else if (EqualsCI(type, "text")) {
    if (EqualsCI(subtype, "markdown")) {
      if (markdown_q_ >= 0 && markdown_q_ != q) return false;
      markdown_q_ = q;
    } else if (EqualsCI(subtype, "html")) {
      KeepMax(q, &html_q_);
    } else if (subtype == "*") {
      KeepMax(q, &text_any_q_);
    }
  }
  return true;
}

void AcceptMarkdownPreference::AddLine(std::string_view line) {
  size_t pos = 0;
  while (!malformed_) {
    // Empty list elements are allowed (RFC 9110 section 5.6.1).
    SkipOws(line, &pos);
    while (pos < line.size() && line[pos] == ',') {
      ++pos;
      SkipOws(line, &pos);
    }
    if (pos >= line.size()) return;
    if (!ParseElement(line, &pos)) {
      malformed_ = true;
      return;
    }
    SkipOws(line, &pos);
    if (pos >= line.size()) return;
    if (line[pos] != ',') malformed_ = true;
  }
}

bool AcceptMarkdownPreference::PrefersMarkdown() const {
  if (malformed_ || markdown_q_ <= 0) return false;
  int html_q = 0;
  if (html_q_ >= 0) {
    html_q = html_q_;
  } else if (text_any_q_ >= 0) {
    html_q = text_any_q_;
  } else if (any_q_ >= 0) {
    html_q = any_q_;
  }
  return markdown_q_ >= html_q;
}

bool AcceptPrefersMarkdown(std::span<const std::string_view> accept_lines) {
  AcceptMarkdownPreference preference;
  for (std::string_view line : accept_lines) preference.AddLine(line);
  return preference.PrefersMarkdown();
}

bool WantsAgentMarkdown(std::string_view accept) {
  // Kept SEPARATE from the CapabilityMask (which has no free bit) so the agent
  // intent is its own signal.
  AcceptMarkdownPreference preference;
  preference.AddLine(accept);
  return preference.PrefersMarkdown();
}

CapabilityMask CapabilityMask::FromHeaders(std::string_view accept,
                                           std::string_view user_agent,
                                           std::string_view save_data,
                                           std::string_view accept_encoding,
                                           std::string_view sec_ch_dpr) {
  CapabilityMask mask;

  // Parse image format from Accept header
  // Priority: AVIF > WebP > image/* → WebP > */* → WebP >
  //           absent Accept → */* → WebP > Original
  // Note: image/jxl in Accept maps to kOriginal (JXL support dropped;
  // format bits 11 repurposed for kSvg which is worker-only, never set
  // from client headers).
  if (AcceptContains(accept, "image/avif")) {
    mask.image_format_ = ImageFormat::kAvif;
  } else if (accept.empty() || AcceptContains(accept, "image/webp") ||
             AcceptContains(accept, "image/*") ||
             AcceptContains(accept, "*/*")) {
    // image/webp: explicit WebP support.
    // image/*: Per RFC 9110 §12.5.1, accepts any image subtype including WebP.
    //   Safari/WebKit supports WebP since v14 but doesn't list it explicitly.
    // */*: Strictly subsumes image/*. Chrome on iOS (CriOS/WKWebView) sends
    //   Accept: */* for image subresource requests without listing image types.
    // absent Accept (empty view): RFC 9110 §12.5.1 — "A request without any
    //   Accept header field implies that the user agent will accept any media
    //   type in response", i.e. it is semantically */*, and */* is already
    //   handled one clause up. Classifying it anywhere else would make the
    //   absent-header shape the single request shape in the whole space that
    //   resolves differently from the wildcard it stands for (decided in
    //   #1377). The embedder collapses "header absent" and "header present
    //   with an empty value" to the same empty view, so both land here; an
    //   empty field-value carries no media range to honour either way.
    //   Note this deliberately does NOT extend to WantsAgentMarkdown:
    //   */* does not opt into markdown, so neither does an absent Accept.
    //   Cross-reference: the format field this sets is a negotiated
    //   PREFERENCE, and ScoreAlternate hard-disqualifies a stored variant
    //   whose format it does not name (#1331, lib/classify/pagespeed_selector.cc).
    //   Widening the absent-header shape from Original to WebP is monotone
    //   under that rule -- a stored original-format alternate still earns the
    //   original-format fallback bonus at the WebP mask, and the WebP slots
    //   become reachable in addition -- which is why it is safe here and was
    //   decided on the issue rather than in the selector.
    // In all cases, serve WebP as the most efficient universally-supported
    // format. Vary: Accept protects proxy caches.
    mask.image_format_ = ImageFormat::kWebP;
  } else {
    // A present-but-non-matching Accept is an explicit, narrower statement of
    // what the client takes — image/jxl → kOriginal (JXL slot repurposed), or
    // an unrecognized format. Unlike the absent header, it is not a wildcard.
    mask.image_format_ = ImageFormat::kOriginal;
  }

  // Parse viewport from User-Agent
  mask.viewport_ = ParseViewport(user_agent);

  // Pixel density: an explicit Sec-CH-DPR client hint wins over UA
  // sniffing.  Without a hint, mobile viewports default to 2x — modern
  // phones are uniformly >= 2x and don't carry density markers in the
  // UA.  "2x"/"Retina" UA markers remain a positive signal for any
  // viewport class.
  auto dpr_high = DprHintIsHighDensity(sec_ch_dpr);
  if (dpr_high.has_value()) {
    mask.pixel_density_ = *dpr_high ? PixelDensity::k2xPlus : PixelDensity::k1x;
  } else if (mask.viewport_ == Viewport::kMobile ||
             user_agent.find("2x") != std::string_view::npos ||
             user_agent.find("Retina") != std::string_view::npos) {
    mask.pixel_density_ = PixelDensity::k2xPlus;
  }

  // Parse Save-Data header (case-insensitive, no allocation).
  if (!save_data.empty()) {
    if (EqualsCI(save_data, "on") || save_data == "1" ||
        EqualsCI(save_data, "true")) {
      mask.save_data_ = SaveData::kOn;
    }
  }

  // Parse Accept-Encoding header
  mask.transfer_encoding_ = ParseAcceptEncoding(accept_encoding);

  return mask;
}

uint32_t CapabilityMask::Encode() const {
  uint32_t encoded = 0;
  encoded |= (static_cast<uint32_t>(image_format_) & kImageFormatMask)
             << kImageFormatShift;
  encoded |= (static_cast<uint32_t>(viewport_) & kViewportMask)
             << kViewportShift;
  encoded |= (static_cast<uint32_t>(pixel_density_) & kPixelDensityMask)
             << kPixelDensityShift;
  encoded |= (static_cast<uint32_t>(save_data_) & kSaveDataMask)
             << kSaveDataShift;
  encoded |= (static_cast<uint32_t>(transfer_encoding_) & kEncodingMask)
             << kEncodingShift;
  return encoded;
}

CapabilityMask CapabilityMask::Decode(uint32_t encoded) {
  CapabilityMask mask;
  mask.image_format_ = static_cast<ImageFormat>((encoded >> kImageFormatShift) &
                                                kImageFormatMask);
  mask.viewport_ =
      static_cast<Viewport>((encoded >> kViewportShift) & kViewportMask);
  mask.pixel_density_ = static_cast<PixelDensity>(
      (encoded >> kPixelDensityShift) & kPixelDensityMask);
  mask.save_data_ =
      static_cast<SaveData>((encoded >> kSaveDataShift) & kSaveDataMask);
  mask.transfer_encoding_ = static_cast<TransferEncoding>(
      (encoded >> kEncodingShift) & kEncodingMask);
  return mask;
}

CapabilityMask::ViewportRange CapabilityMask::ViewportWidthRange(Viewport vp) {
  switch (vp) {
    case Viewport::kMobile:
      return {0, 479};
    case Viewport::kTablet:
      return {480, 1279};
    case Viewport::kDesktop:
      return {1280, 65535};
  }
  return {1280, 65535};  // Default to desktop
}

bool CapabilityMask::operator==(const CapabilityMask& other) const {
  return image_format_ == other.image_format_ && viewport_ == other.viewport_ &&
         pixel_density_ == other.pixel_density_ &&
         save_data_ == other.save_data_ &&
         transfer_encoding_ == other.transfer_encoding_;
}

}  // namespace pagespeed

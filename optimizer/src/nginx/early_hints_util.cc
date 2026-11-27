// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP 103 Early Hints Utilities

#include "src/nginx/early_hints_util.h"

#include "lib/base/string_util.h"

namespace pagespeed {

using net_instaweb::LowerChar;

namespace {

// Check if a percent-encoded sequence at pos matches a target hex pair
// (case-insensitive).  E.g., IsHexPair("0D", ...) matches %0d, %0D.
bool IsHexPair(std::string_view hex, char hi, char lo) {
  return hex.size() == 2 && LowerChar(hex[0]) == hi && LowerChar(hex[1]) == lo;
}

}  // namespace

bool ShouldSendHintsOnMiss(std::string_view url_mime, std::string_view accept) {
  if (!url_mime.empty()) {
    return url_mime == "text/html";
  }
  // No recognized extension: key on what the client asks for.
  for (size_t i = 0; i + 9 <= accept.size(); ++i) {
    size_t j = 0;
    constexpr std::string_view kHtml = "text/html";
    while (j < kHtml.size() && LowerChar(accept[i + j]) == kHtml[j]) ++j;
    if (j == kHtml.size()) return true;
  }
  return false;
}

FlushStatus FlushWithRetry(const std::function<long()>& send_remaining,
                           int max_stalls) {
  long prev_pending = -1;  // sentinel: no prior observation yet
  int stalls = 0;
  for (;;) {
    long pending = send_remaining();
    if (pending < 0) {
      return FlushStatus::kError;
    }
    if (pending == 0) {
      return FlushStatus::kComplete;
    }
    // Partial write. Count it as a stall only if we made no progress versus
    // the previous observation; any forward progress resets the budget.
    if (prev_pending >= 0 && pending >= prev_pending) {
      if (++stalls >= max_stalls) {
        return FlushStatus::kWouldBlock;
      }
    } else {
      stalls = 0;
    }
    prev_pending = pending;
  }
}

std::string SanitizeLinkUrl(std::string_view url) {
  // Reject URLs with percent-encoded dangerous characters.
  for (size_t i = 0; i + 2 < url.size(); ++i) {
    if (url[i] == '%') {
      auto hex = url.substr(i + 1, 2);
      if (IsHexPair(hex, '0', 'd') || IsHexPair(hex, '0', 'a') ||
          IsHexPair(hex, '0', '0')) {
        return {};
      }
    }
  }

  // Strip raw dangerous characters.
  std::string result;
  result.reserve(url.size());
  for (char c : url) {
    if (c != '\r' && c != '\n' && c != '\0' && c != '<' && c != '>') {
      result.push_back(c);
    }
  }
  return result;
}

// Strip CR/LF from a header field value to prevent header injection.
std::string SanitizeHeaderValue(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (char c : value) {
    if (c != '\r' && c != '\n') {
      result.push_back(c);
    }
  }
  return result;
}

std::string FormatLinkHeader(const std::vector<PreloadResource>& resources) {
  std::string result;
  for (const auto& resource : resources) {
    std::string sanitized = SanitizeLinkUrl(resource.url);
    if (sanitized.empty()) {
      continue;  // Empty URL or rejected by sanitization.
    }
    if (!result.empty()) {
      result.append(", ");
    }
    result.append("<");
    result.append(sanitized);
    result.append(">; rel=");
    std::string rel =
        SanitizeHeaderValue(resource.rel.empty() ? "preload" : resource.rel);
    result.append(rel);
    if (!resource.as_type.empty()) {
      result.append("; as=");
      result.append(SanitizeHeaderValue(resource.as_type));
    }
    if (!resource.extra.empty()) {
      result.append("; ");
      result.append(SanitizeHeaderValue(resource.extra));
    }
  }
  return result;
}

PreloadResource ParseHintLine(std::string_view line) {
  constexpr std::string_view kImagePrefix = "image:";
  constexpr std::string_view kFontPrefix = "font:";
  constexpr std::string_view kPreconnectPrefix = "preconnect:";
  constexpr std::string_view kPreconnectCorsPrefix = "preconnect-cors:";
  if (line.starts_with(kImagePrefix)) {
    return {std::string(line.substr(kImagePrefix.size())), "image", "preload",
            "fetchpriority=high"};
  }
  if (line.starts_with(kFontPrefix)) {
    return {std::string(line.substr(kFontPrefix.size())), "font", "preload",
            "crossorigin"};
  }
  if (line.starts_with(kPreconnectPrefix)) {
    return {std::string(line.substr(kPreconnectPrefix.size())), "",
            "preconnect", ""};
  }
  // CORS-mode origin: the preconnect must carry crossorigin so it warms the
  // same connection pool the motivating resource (font, crossorigin script,
  // ES module) will use.
  if (line.starts_with(kPreconnectCorsPrefix)) {
    return {std::string(line.substr(kPreconnectCorsPrefix.size())), "",
            "preconnect", "crossorigin"};
  }
  return {std::string(line), "style", "preload", ""};
}

std::vector<PreloadResource> ExtractStylesheetPreloads(std::string_view html) {
  std::vector<PreloadResource> preloads;

  size_t pos = 0;
  while ((pos = html.find("<link", pos)) != std::string_view::npos) {
    size_t tag_end = html.find('>', pos);
    if (tag_end == std::string_view::npos) break;

    std::string_view tag = html.substr(pos, tag_end - pos + 1);

    // Check if this is a stylesheet link.
    if (tag.find("stylesheet") != std::string_view::npos) {
      // Extract href value.
      size_t href_pos = tag.find("href=");
      if (href_pos != std::string_view::npos) {
        href_pos += 5;  // skip "href="
        char quote = '\0';
        if (href_pos < tag.size() &&
            (tag[href_pos] == '"' || tag[href_pos] == '\'')) {
          quote = tag[href_pos];
          href_pos++;
        }
        size_t href_end;
        if (quote != '\0') {
          href_end = tag.find(quote, href_pos);
        } else {
          href_end = tag.find_first_of(" >", href_pos);
        }
        if (href_end != std::string_view::npos && href_end > href_pos) {
          std::string href(tag.substr(href_pos, href_end - href_pos));
          // Strip CR/LF to prevent header injection (CRLF attacks).
          std::erase(href, '\r');
          std::erase(href, '\n');
          if (!href.empty()) {
            preloads.push_back({std::move(href), "style", "preload", ""});
          }
        }
      }
    }
    pos = tag_end + 1;
  }

  return preloads;
}

}  // namespace pagespeed

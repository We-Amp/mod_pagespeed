// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/url_norm.h"

#include <algorithm>
#include <cstddef>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"

namespace pagespeed::packs {

std::optional<std::string> NormUrl(std::string_view url_in) {
  // Leading and trailing whitespace is trimmed.
  std::string_view url = absl::StripAsciiWhitespace(url_in);
  const size_t frag = url.find('#');
  if (frag != std::string_view::npos) url = url.substr(0, frag);

  // As in a browser URL parser, a backslash before the query is a '/'.
  std::string fixed(url);
  const size_t qpos = fixed.find('?');
  for (size_t i = 0; i < std::min(qpos, fixed.size()); ++i) {
    if (fixed[i] == '\\') fixed[i] = '/';
  }
  url = fixed;

  std::string scheme;
  if (absl::StartsWithIgnoreCase(url, "https://")) {
    scheme = "https";
    url.remove_prefix(8);
  } else if (absl::StartsWithIgnoreCase(url, "http://")) {
    scheme = "http";
    url.remove_prefix(7);
  } else {
    return std::nullopt;
  }

  const size_t auth_end = url.find_first_of("/?");
  std::string_view authority = url.substr(0, auth_end);
  std::string_view rest =
      auth_end == std::string_view::npos ? "" : url.substr(auth_end);
  if (authority.empty() || authority.find('@') != std::string_view::npos) {
    return std::nullopt;
  }
  for (char c : authority) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
      return std::nullopt;
    }
  }

  std::string_view host = authority;
  std::string_view port;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = authority.substr(0, close + 1);
    std::string_view after = authority.substr(close + 1);
    if (!after.empty()) {
      if (after.front() != ':') return std::nullopt;
      port = after.substr(1);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }
  }
  if (host.empty()) return std::nullopt;

  std::string port_out;
  if (!port.empty()) {
    unsigned int p = 0;
    if (!absl::SimpleAtoi(port, &p) || p > 65535) return std::nullopt;
    if (!((scheme == "http" && p == 80) || (scheme == "https" && p == 443))) {
      port_out = absl::StrCat(":", p);
    }
  }

  std::string_view path = rest;
  std::string_view query;
  const size_t q = rest.find('?');
  if (q != std::string_view::npos) {
    path = rest.substr(0, q);
    query = rest.substr(q);             // keeps the leading '?'
    if (query.size() == 1) query = {};  // a bare trailing '?' is no query
  }
  if (!path.empty() && path.back() == '/') path.remove_suffix(1);

  return absl::StrCat(scheme, "://", absl::AsciiStrToLower(host), port_out,
                      path, query);
}

std::optional<std::string> NormalizeHreflangCode(std::string_view code) {
  std::string c = absl::AsciiStrToLower(code);
  for (char& ch : c) {
    if (ch == '_') ch = '-';
  }
  if (c == "x-default") return c;

  auto is_alpha = [](std::string_view s) {
    for (char ch : s) {
      if (ch < 'a' || ch > 'z') return false;
    }
    return true;
  };
  auto is_digit = [](std::string_view s) {
    for (char ch : s) {
      if (ch < '0' || ch > '9') return false;
    }
    return true;
  };

  std::string_view v = c;
  size_t dash = v.find('-');
  std::string_view lang = v.substr(0, dash);
  if (lang.size() < 2 || lang.size() > 3 || !is_alpha(lang)) {
    return std::nullopt;
  }
  if (dash == std::string_view::npos) return c;
  v.remove_prefix(dash + 1);

  // Optional 4-letter script, then optional 2-letter or 3-digit region.
  dash = v.find('-');
  std::string_view part = v.substr(0, dash);
  if (part.size() == 4 && is_alpha(part)) {
    if (dash == std::string_view::npos) return c;
    v.remove_prefix(dash + 1);
    dash = v.find('-');
    part = v.substr(0, dash);
  }
  if (dash != std::string_view::npos) return std::nullopt;
  if ((part.size() == 2 && is_alpha(part)) ||
      (part.size() == 3 && is_digit(part))) {
    return c;
  }
  return std::nullopt;
}

}  // namespace pagespeed::packs

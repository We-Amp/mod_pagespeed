// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/hostname.h"

#include "lib/base/string_util.h"

namespace pagespeed {

std::string NormalizeHostname(std::string_view hostname) {
  if (hostname.empty()) {
    return {};
  }

  // Find the port separator (last colon, but not inside brackets for IPv6).
  // For simplicity, only handle the common case: host:port or host.
  std::string_view host_part = hostname;
  std::string_view port_part;

  // Check for IPv6 bracket notation: [::1]:80
  if (!hostname.empty() && hostname.front() == '[') {
    auto bracket_end = hostname.find(']');
    if (bracket_end != std::string_view::npos &&
        bracket_end + 1 < hostname.size() && hostname[bracket_end + 1] == ':') {
      host_part = hostname.substr(0, bracket_end + 1);
      port_part = hostname.substr(bracket_end + 2);
    }
  } else {
    auto colon = hostname.rfind(':');
    if (colon != std::string_view::npos) {
      // Only treat as host:port if the part after the colon is all digits
      // (avoids misinterpreting un-bracketed IPv6 addresses like "::1").
      auto candidate_port = hostname.substr(colon + 1);
      bool is_port = !candidate_port.empty();
      for (char c : candidate_port) {
        if (c < '0' || c > '9') {
          is_port = false;
          break;
        }
      }
      if (is_port) {
        host_part = hostname.substr(0, colon);
        port_part = candidate_port;
      }
    }
  }

  // Lowercase the host part (ASCII only)
  std::string result;
  result.reserve(host_part.size() + port_part.size() + 1);
  for (char c : host_part) {
    result.push_back(net_instaweb::LowerChar(c));
  }

  // Strip trailing dot
  if (!result.empty() && result.back() == '.') {
    result.pop_back();
  }

  // Append port if non-default
  if (!port_part.empty() && port_part != "80" && port_part != "443") {
    result.push_back(':');
    result.append(port_part);
  }

  return result;
}

}  // namespace pagespeed

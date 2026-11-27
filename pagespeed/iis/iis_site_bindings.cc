// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_site_bindings.h"

namespace net_instaweb {

namespace {

bool IsDigits(StringPiece s) {
  if (s.empty() || s.size() > 5) return false;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// A host header name as IIS binds it: letters, digits, '-', '.' and '_'.
bool IsBindableHost(StringPiece host) {
  if (host.empty()) return false;
  for (const char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                    c == '_';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool IisBindingHostName(StringPiece protocol, StringPiece binding_information,
                        GoogleString* host) {
  host->clear();
  if (!StringCaseEqual(protocol, "http") &&
      !StringCaseEqual(protocol, "https")) {
    return false;
  }
  StringPiece rest = binding_information;
  // The address: "*", an IPv4 address, or a bracketed IPv6 one.
  size_t address_end;
  if (!rest.empty() && rest[0] == '[') {
    const size_t close = rest.find(']');
    if (close == StringPiece::npos) return false;
    address_end = close + 1;
    if (address_end >= rest.size() || rest[address_end] != ':') return false;
  } else {
    address_end = rest.find(':');
    if (address_end == StringPiece::npos) return false;
  }
  rest.remove_prefix(address_end + 1);
  const size_t port_end = rest.find(':');
  if (port_end == StringPiece::npos) return false;
  if (!IsDigits(rest.substr(0, port_end))) return false;
  rest.remove_prefix(port_end + 1);
  if (!IsBindableHost(rest)) return false;
  rest.CopyToString(host);
  return true;
}

ConfiguredHostNames IisSiteHostNames(
    const std::vector<std::pair<GoogleString, GoogleString>>& bindings) {
  ConfiguredHostNames names;
  std::vector<GoogleString> seen;
  for (const auto& [protocol, information] : bindings) {
    GoogleString host;
    if (!IisBindingHostName(protocol, information, &host)) continue;
    LowerString(&host);
    bool known = false;
    for (const GoogleString& s : seen) known = known || s == host;
    if (known) continue;
    seen.push_back(host);
    if (names.primary.empty()) {
      names.primary = host;
    } else {
      names.aliases.push_back(host);
    }
  }
  return names;
}

}  // namespace net_instaweb

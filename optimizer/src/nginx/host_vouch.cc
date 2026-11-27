// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/host_vouch.h"

namespace pagespeed {

namespace {

char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
  }
  return true;
}

}  // namespace

std::string AuthorityHost(std::string_view authority) {
  std::string_view host = authority;
  if (!host.empty() && host.front() == '[') {
    size_t close = host.find(']');
    host = (close == std::string_view::npos) ? std::string_view()
                                             : host.substr(0, close + 1);
  } else {
    size_t colon = host.find(':');
    if (colon != std::string_view::npos) host = host.substr(0, colon);
  }
  std::string out;
  out.reserve(host.size());
  for (char c : host) out.push_back(LowerAscii(c));
  if (!out.empty() && out.back() == '.') out.pop_back();
  return out;
}

bool ServerNameMatchesHost(std::string_view server_name,
                           std::string_view host) {
  if (server_name.empty() || host.empty()) return false;
  if (server_name.size() > 2 && server_name.substr(0, 2) == "*.") {
    // "*.example.com": at least one label before ".example.com".
    std::string_view suffix = server_name.substr(1);  // ".example.com"
    return host.size() > suffix.size() &&
           EqualsIgnoreCase(host.substr(host.size() - suffix.size()), suffix);
  }
  if (server_name.size() > 1 && server_name.front() == '.') {
    // ".example.com": example.com itself, or any name below it.
    std::string_view base = server_name.substr(1);
    if (EqualsIgnoreCase(host, base)) return true;
    return host.size() > server_name.size() &&
           EqualsIgnoreCase(host.substr(host.size() - server_name.size()),
                            server_name);
  }
  if (server_name.size() > 2 &&
      server_name.substr(server_name.size() - 2) == ".*") {
    // "www.example.*": at least one more label after "www.example.".
    std::string_view prefix =
        server_name.substr(0, server_name.size() - 1);  // "www.example."
    return host.size() > prefix.size() &&
           EqualsIgnoreCase(host.substr(0, prefix.size()), prefix);
  }
  return EqualsIgnoreCase(server_name, host);
}

bool HostAliasesName(
    const std::unordered_map<std::string, std::string>& host_aliases,
    std::string_view hostname) {
  if (hostname.empty()) return false;
  for (const auto& [alias, target] : host_aliases) {
    if (alias == hostname || target == hostname) return true;
  }
  return false;
}

}  // namespace pagespeed

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/upstream_pin.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>  // inet_pton, inet_ntop, AF_INET, AF_INET6
#endif

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/net/ssrf_guard.h"

namespace pagespeed {
namespace {

// Canonicalize a host for byte-equal comparison: strip a single trailing root dot
// and, for an IP literal, normalize the text via inet_pton+inet_ntop so different
// spellings of the same address match (e.g. "::1" == "0:0:0:0:0:0:0:1", and the
// already-dotted IPv4 ParseHttpUrl produced is idempotent here). Hostnames are
// already lower-cased by ParseHttpUrl. Returns empty on failure.
std::string CanonHost(std::string_view host, bool is_ip_literal) {
  std::string h(host);
  if (!h.empty() && h.back() == '.') h.pop_back();  // trailing FQDN root dot
  if (h.empty()) return h;
  if (is_ip_literal) {
    unsigned char buf[16];
    char out[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, h.c_str(), buf) == 1 &&
        inet_ntop(AF_INET, buf, out, sizeof(out)) != nullptr) {
      return out;
    }
    std::string h6 = h;
    const size_t pct = h6.find('%');  // strip zone id before canonicalizing
    if (pct != std::string::npos) h6.resize(pct);
    if (!h6.empty() && inet_pton(AF_INET6, h6.c_str(), buf) == 1 &&
        inet_ntop(AF_INET6, buf, out, sizeof(out)) != nullptr) {
      return out;
    }
  }
  return h;
}

// Canonicalize a parsed authority into a PinnedUpstream, enforcing the G1 input
// rules: NO userinfo (a page cannot smuggle `upstream@evil`/`evil@upstream`), an
// http(s) scheme, and a known port. nullopt on any violation.
std::optional<PinnedUpstream> Canonicalize(const UrlAuthority& a) {
  if (a.had_userinfo) return std::nullopt;  // §3.3 G1.1
  if (a.scheme != "http" && a.scheme != "https") return std::nullopt;
  if (a.port <= 0) return std::nullopt;  // need explicit port
  PinnedUpstream u;
  u.scheme = a.scheme;
  u.port = a.port;
  u.is_ip_literal = a.host_is_ip_literal;
  u.canon_host = CanonHost(a.host, a.host_is_ip_literal);
  if (u.canon_host.empty()) return std::nullopt;
  return u;
}

}  // namespace

std::optional<PinnedUpstream> ParseUpstream(std::string_view config_url) {
  std::optional<UrlAuthority> a = ParseHttpUrl(config_url);
  if (!a.has_value()) return std::nullopt;
  return Canonicalize(*a);
}

bool IsPinnedUpstream(std::string_view subresource_url,
                      const std::vector<PinnedUpstream>& pinned) {
  std::optional<UrlAuthority> a = ParseHttpUrl(subresource_url);
  if (!a.has_value()) return false;
  std::optional<PinnedUpstream> c = Canonicalize(*a);
  if (!c.has_value())
    return false;  // userinfo / bad scheme / no port → not pinned
  for (const PinnedUpstream& p : pinned) {
    if (p.scheme == c->scheme && p.port == c->port &&
        p.is_ip_literal == c->is_ip_literal && p.canon_host == c->canon_host) {
      return true;  // exact (scheme,host,port,is-literal) equality — never substring
    }
  }
  return false;
}

}  // namespace pagespeed

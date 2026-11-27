// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "src/nginx/agent_self_fetch.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <string_view>

namespace pagespeed {

namespace {

constexpr unsigned char kMappedPrefix[12] = {0, 0, 0, 0, 0,    0,
                                             0, 0, 0, 0, 0xff, 0xff};

// An IPv4-mapped IPv6 address reads as its IPv4 form; anything that is not 4
// or 16 bytes is no address (empty).
std::span<const unsigned char> Canonical(std::span<const unsigned char> a) {
  if (a.size() == 16 && std::equal(std::begin(kMappedPrefix),
                                   std::end(kMappedPrefix), a.begin())) {
    return a.subspan(12, 4);
  }
  if (a.size() == 4 || a.size() == 16) {
    return a;
  }
  return {};
}

bool IsLoopback(std::span<const unsigned char> a) {
  if (a.size() == 4) {
    return a[0] == 127;
  }
  if (a.size() == 16) {
    for (std::size_t i = 0; i < 15; ++i) {
      if (a[i] != 0) {
        return false;
      }
    }
    return a[15] == 1;
  }
  return false;
}

}  // namespace

bool IsAgentFetchUserAgent(std::string_view user_agent) {
  return user_agent.find(kAgentFetchUserAgentMarker) != std::string_view::npos;
}

bool AgentFetchPeerIsThisServer(std::span<const unsigned char> peer,
                                std::span<const unsigned char> local) {
  std::span<const unsigned char> p = Canonical(peer);
  if (p.empty()) {
    return false;
  }
  if (IsLoopback(p)) {
    return true;
  }
  std::span<const unsigned char> l = Canonical(local);
  return !l.empty() && l.size() == p.size() &&
         std::equal(p.begin(), p.end(), l.begin());
}

bool IsAgentSelfFetch(std::string_view user_agent,
                      std::span<const unsigned char> peer,
                      std::span<const unsigned char> local) {
  return IsAgentFetchUserAgent(user_agent) &&
         AgentFetchPeerIsThisServer(peer, local);
}

}  // namespace pagespeed

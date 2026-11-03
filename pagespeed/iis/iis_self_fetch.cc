// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_self_fetch.h"

#include <atomic>
#include <cstring>

#include "base/logging.h"
#include "net/instaweb/public/global_constants.h"

namespace net_instaweb {

const char kIisSelfFetchClientIp[] = "224.0.0.0";

namespace {

// The relabel failed, so the request keeps its real client address; says so
// once per process and returns false.
bool RelabelFailed() {
  static std::atomic_flag warned = ATOMIC_FLAG_INIT;
  if (!warned.test_and_set()) {
    LOG(WARNING) << "PageSpeed: could not relabel the client address of the "
                    "module's own fetch; it keeps its real address";
  }
  return false;
}

// The IPv4 address `addr` stands for, in network byte order, when it is an
// AF_INET address or an IPv4-mapped AF_INET6 address (::ffff:a.b.c.d).
bool AsIpv4(const SOCKADDR* addr, IN_ADDR* out) {
  if (addr->sa_family == AF_INET) {
    *out = reinterpret_cast<const SOCKADDR_IN*>(addr)->sin_addr;
    return true;
  }
  if (addr->sa_family == AF_INET6) {
    const IN6_ADDR& a = reinterpret_cast<const SOCKADDR_IN6*>(addr)->sin6_addr;
    for (int b = 0; b < 10; ++b) {
      if (a.s6_addr[b] != 0) return false;
    }
    if (a.s6_addr[10] != 0xff || a.s6_addr[11] != 0xff) return false;
    memcpy(out, &a.s6_addr[12], sizeof(*out));
    return true;
  }
  return false;
}

bool IsLoopback(const SOCKADDR* addr) {
  IN_ADDR v4;
  if (AsIpv4(addr, &v4)) {
    return reinterpret_cast<const unsigned char*>(&v4)[0] == 127;
  }
  if (addr->sa_family == AF_INET6) {
    const IN6_ADDR& a = reinterpret_cast<const SOCKADDR_IN6*>(addr)->sin6_addr;
    for (int b = 0; b < 15; ++b) {
      if (a.s6_addr[b] != 0) return false;
    }
    return a.s6_addr[15] == 1;
  }
  return false;
}

}  // namespace

bool IisSelfFetchPeerIsThisServer(const SOCKADDR* peer, const SOCKADDR* local) {
  if (peer == nullptr) {
    return false;
  }
  if (IsLoopback(peer)) {
    return true;
  }
  if (local == nullptr) {
    return false;
  }
  IN_ADDR p4, l4;
  const bool peer_v4 = AsIpv4(peer, &p4);
  const bool local_v4 = AsIpv4(local, &l4);
  if (peer_v4 || local_v4) {
    return peer_v4 && local_v4 && p4.s_addr == l4.s_addr;
  }
  if (peer->sa_family == AF_INET6 && local->sa_family == AF_INET6) {
    const auto* p = reinterpret_cast<const SOCKADDR_IN6*>(peer);
    const auto* l = reinterpret_cast<const SOCKADDR_IN6*>(local);
    return memcmp(&p->sin6_addr, &l->sin6_addr, sizeof(p->sin6_addr)) == 0;
  }
  return false;
}

bool IisIsSelfFetch(StringPiece user_agent, const SOCKADDR* peer,
                    const SOCKADDR* local) {
  // WinHTTP sends "WinHttp mod_pagespeed " (asyncwinhttp.cpp); match on the
  // marker substring, as the request handler does.
  if (user_agent.find(kModPagespeedSubrequestUserAgent) == StringPiece::npos) {
    return false;
  }
  return IisSelfFetchPeerIsThisServer(peer, local);
}

bool IisRelabelSelfFetch(SOCKADDR* remote) {
  if (remote == nullptr) {
    return RelabelFailed();
  }
  size_t size;
  if (remote->sa_family == AF_INET) {
    size = sizeof(SOCKADDR_IN);
  } else if (remote->sa_family == AF_INET6) {
    size = sizeof(SOCKADDR_IN6);
  } else {
    return RelabelFailed();
  }
  memset(remote, 0, size);
  SOCKADDR_IN* v4 = reinterpret_cast<SOCKADDR_IN*>(remote);
  v4->sin_family = AF_INET;
  v4->sin_port = 0;
  v4->sin_addr.s_addr = htonl(0xe0000000u);  // 224.0.0.0
  return true;
}

}  // namespace net_instaweb

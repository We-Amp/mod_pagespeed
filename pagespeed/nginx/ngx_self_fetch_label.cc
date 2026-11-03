// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_self_fetch_label.h"

#include <arpa/inet.h>

#include <cstring>

#include "pagespeed/system/loopback_route_fetcher.h"

namespace net_instaweb {

const char kNgxSelfFetchLabelText[] = "224.0.0.0";

bool NgxSelfFetchPeerIsThisServer(const struct sockaddr* peer,
                                  const struct sockaddr* local) {
  if (peer == nullptr) {
    return false;
  }
  if (LoopbackRouteFetcher::IsLoopbackAddr(peer)) {
    return true;
  }
  if (local == nullptr || local->sa_family != peer->sa_family) {
    return false;
  }
  if (peer->sa_family == AF_INET) {
    const auto* p = reinterpret_cast<const struct sockaddr_in*>(peer);
    const auto* l = reinterpret_cast<const struct sockaddr_in*>(local);
    return p->sin_addr.s_addr == l->sin_addr.s_addr;
  }
  if (peer->sa_family == AF_INET6) {
    const auto* p = reinterpret_cast<const struct sockaddr_in6*>(peer);
    const auto* l = reinterpret_cast<const struct sockaddr_in6*>(local);
    return memcmp(&p->sin6_addr, &l->sin6_addr, sizeof(p->sin6_addr)) == 0;
  }
  return false;
}

void NgxInitSelfFetchLabel(NgxSelfFetchLabel* label) {
  memset(label, 0, sizeof(*label));
  label->addr.sin_family = AF_INET;
  label->addr.sin_port = 0;
  label->addr.sin_addr.s_addr = htonl(0xe0000000u);  // 224.0.0.0
  static_assert(sizeof(label->text) == sizeof(kNgxSelfFetchLabelText),
                "label text buffer matches the label");
  memcpy(label->text, kNgxSelfFetchLabelText, sizeof(label->text));
}

}  // namespace net_instaweb

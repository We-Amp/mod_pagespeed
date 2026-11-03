// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/apache/apache_self_fetch.h"

#include <cstring>

#include "ap_release.h"   // NOLINT
#include "apr_strings.h"  // NOLINT
#include "apr_tables.h"   // NOLINT
#include "base/logging.h"
#include "net/instaweb/public/global_constants.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/loopback_route_fetcher.h"

#if (AP_SERVER_MAJORVERSION_NUMBER == 2) && (AP_SERVER_MINORVERSION_NUMBER >= 4)
#define MPS_SELF_FETCH_APACHE_24
#endif

namespace net_instaweb {

const char kApacheSelfFetchClientIp[] = "224.0.0.0";

bool ApacheSelfFetchPeerIsThisServer(const apr_sockaddr_t* peer,
                                     const apr_sockaddr_t* local) {
  if (peer == nullptr) {
    return false;
  }
  if (LoopbackRouteFetcher::IsLoopbackAddr(
          reinterpret_cast<const struct sockaddr*>(&peer->sa))) {
    return true;
  }
  return local != nullptr && apr_sockaddr_equal(peer, local) != 0;
}

bool ApacheTreatSelfFetchAsRemote(request_rec* r) {
  // The curl fetcher produces user-agent strings like
  // "CurlPagespeed (mod_pagespeed/1.1.0-beta.1-hash)", so we match on the
  // kModPagespeedSubrequestUserAgent substring.
  const char* ua = apr_table_get(r->headers_in, HttpAttributes::kUserAgent);
  if (ua == nullptr ||
      strstr(ua, kModPagespeedSubrequestUserAgent) == nullptr) {
    return false;
  }
  conn_rec* c = r->connection;
  // The connection's own peer, never a client address another module took
  // from a forwarding header: the fetch connects to this server directly.
#ifdef MPS_SELF_FETCH_APACHE_24
  const apr_sockaddr_t* peer = c->client_addr;
#else
  const apr_sockaddr_t* peer = c->remote_addr;
#endif
  if (!ApacheSelfFetchPeerIsThisServer(peer, c->local_addr)) {
    return false;
  }

  // The rewrite is per request, so it is allocated from the request pool:
  // the connection pool lives as long as the keep-alive connection, and
  // every self-fetch on one would grow it.
  apr_sockaddr_t* untrusted_sockaddr = nullptr;
  CHECK_EQ(APR_SUCCESS,
           apr_sockaddr_info_get(&untrusted_sockaddr, kApacheSelfFetchClientIp,
                                 APR_INET, 80, 0, r->pool));
  char* untrusted_ip_str = apr_pstrdup(r->pool, kApacheSelfFetchClientIp);
#ifdef MPS_SELF_FETCH_APACHE_24
  r->useragent_ip = untrusted_ip_str;
  r->useragent_addr = untrusted_sockaddr;
#else
  c->remote_ip = untrusted_ip_str;
  c->remote_addr = untrusted_sockaddr;
#endif

  // An empty remote host name is what Apache records when the lookup fails,
  // so it passes no host-name rule either.  remote_host is a conn_rec field
  // that outlives this request (httpd reads it between keep-alive requests,
  // e.g. from the scoreboard), so it must not point into r->pool: use a
  // static string, as httpd's core does.
  c->remote_host = const_cast<char*>("");
  return true;
}

}  // namespace net_instaweb

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// A request carrying the module's fetch marker that comes from this server
// (over loopback, or from the address it arrived on) is relabeled to a
// reserved non-local address before the access hooks run; a request from any
// other address, or without the marker, is left as it is.

#include "pagespeed/apache/apache_self_fetch.h"

#include <cstring>

#include "apr_network_io.h"  // NOLINT
#include "apr_pools.h"       // NOLINT
#include "apr_tables.h"      // NOLINT
#include "pagespeed/kernel/http/http_names.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Marker shapes the module's own fetchers send, and one a client could send.
const char kFetcherUserAgent[] = "CurlPagespeed (mod_pagespeed/9.9.9-t)";
const char kBrowserUserAgent[] = "ExampleBrowser/3.1";

class ApacheSelfFetchTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();
    MockApache::PrepareRequest(&request_);
    memset(&connection_, 0, sizeof(connection_));
    request_.connection = &connection_;
    connection_.remote_host = const_cast<char*>("peer.example.test");
  }

  void TearDown() override {
    MockApache::CleanupRequest(&request_);
    MockApache::Terminate();
  }

  apr_sockaddr_t* Addr(const char* ip, apr_int32_t family, apr_port_t port) {
    apr_sockaddr_t* addr = nullptr;
    EXPECT_EQ(APR_SUCCESS,
              apr_sockaddr_info_get(&addr, ip, family, port, 0, request_.pool));
    return addr;
  }

  // A connection from `peer` that arrived on `local`; the request's client
  // address starts as the connection's, as httpd sets it.
  void Connect(apr_sockaddr_t* peer, apr_sockaddr_t* local) {
    connection_.client_addr = peer;
    connection_.local_addr = local;
    request_.useragent_addr = peer;
    request_.useragent_ip = const_cast<char*>("peer-ip");
  }

  void SetUserAgent(const char* ua) {
    apr_table_set(request_.headers_in, HttpAttributes::kUserAgent, ua);
  }

  void ExpectUntouched(apr_sockaddr_t* peer) {
    EXPECT_EQ(peer, request_.useragent_addr);
    EXPECT_STREQ("peer-ip", request_.useragent_ip);
    EXPECT_EQ(peer, connection_.client_addr);
    EXPECT_STREQ("peer.example.test", connection_.remote_host);
  }

  void ExpectRelabeled(apr_sockaddr_t* peer) {
    ASSERT_NE(nullptr, request_.useragent_addr);
    EXPECT_STREQ(kApacheSelfFetchClientIp, request_.useragent_ip);
    // The relabeled address is no longer this server, by either reading.
    EXPECT_FALSE(ApacheSelfFetchPeerIsThisServer(request_.useragent_addr,
                                                 connection_.local_addr));
    EXPECT_STREQ("", connection_.remote_host);
    // The connection's own record of its peer is not rewritten.
    EXPECT_EQ(peer, connection_.client_addr);
  }

  request_rec request_;
  conn_rec connection_;
};

TEST_F(ApacheSelfFetchTest, LoopbackPeersAreThisServer) {
  apr_sockaddr_t* local = Addr("192.0.2.10", APR_INET, 8080);
  EXPECT_TRUE(ApacheSelfFetchPeerIsThisServer(
      Addr("127.0.0.1", APR_INET, 40001), local));
  EXPECT_TRUE(ApacheSelfFetchPeerIsThisServer(
      Addr("127.3.4.5", APR_INET, 40001), local));
  EXPECT_TRUE(ApacheSelfFetchPeerIsThisServer(
      Addr("127.0.0.1", APR_INET, 40001), nullptr));
#if APR_HAVE_IPV6
  EXPECT_TRUE(
      ApacheSelfFetchPeerIsThisServer(Addr("::1", APR_INET6, 40001), nullptr));
#endif
}

TEST_F(ApacheSelfFetchTest, PeerOnTheArrivalAddressIsThisServer) {
  // Same address, different ports: the fetch connects from an ephemeral port.
  EXPECT_TRUE(ApacheSelfFetchPeerIsThisServer(
      Addr("192.0.2.10", APR_INET, 40002), Addr("192.0.2.10", APR_INET, 8080)));
}

TEST_F(ApacheSelfFetchTest, OtherPeersAreNotThisServer) {
  apr_sockaddr_t* local = Addr("192.0.2.10", APR_INET, 8080);
  EXPECT_FALSE(ApacheSelfFetchPeerIsThisServer(
      Addr("192.0.2.11", APR_INET, 40003), local));
  EXPECT_FALSE(ApacheSelfFetchPeerIsThisServer(
      Addr("198.51.100.7", APR_INET, 40003), local));
  EXPECT_FALSE(ApacheSelfFetchPeerIsThisServer(
      Addr("198.51.100.7", APR_INET, 40003), nullptr));
  EXPECT_FALSE(ApacheSelfFetchPeerIsThisServer(nullptr, local));
}

TEST_F(ApacheSelfFetchTest, SelfFetchFromTheArrivalAddressIsRelabeled) {
  apr_sockaddr_t* peer = Addr("192.0.2.10", APR_INET, 40004);
  Connect(peer, Addr("192.0.2.10", APR_INET, 8080));
  SetUserAgent(kFetcherUserAgent);
  EXPECT_TRUE(ApacheTreatSelfFetchAsRemote(&request_));
  ExpectRelabeled(peer);
}

TEST_F(ApacheSelfFetchTest, SelfFetchOverLoopbackIsRelabeled) {
  apr_sockaddr_t* peer = Addr("127.0.0.1", APR_INET, 40005);
  Connect(peer, Addr("127.0.0.1", APR_INET, 8080));
  SetUserAgent(kFetcherUserAgent);
  EXPECT_TRUE(ApacheTreatSelfFetchAsRemote(&request_));
  ExpectRelabeled(peer);
}

TEST_F(ApacheSelfFetchTest, RequestWithoutTheMarkerIsLeftAlone) {
  apr_sockaddr_t* peer = Addr("127.0.0.1", APR_INET, 40006);
  Connect(peer, Addr("127.0.0.1", APR_INET, 8080));
  SetUserAgent(kBrowserUserAgent);
  EXPECT_FALSE(ApacheTreatSelfFetchAsRemote(&request_));
  ExpectUntouched(peer);

  apr_table_unset(request_.headers_in, HttpAttributes::kUserAgent);
  EXPECT_FALSE(ApacheTreatSelfFetchAsRemote(&request_));
  ExpectUntouched(peer);
}

TEST_F(ApacheSelfFetchTest, MarkerFromAnotherAddressIsLeftAlone) {
  // A remote client that copies the marker keeps its own address, so a rule
  // that names that address still applies to it.
  apr_sockaddr_t* peer = Addr("198.51.100.7", APR_INET, 40007);
  Connect(peer, Addr("192.0.2.10", APR_INET, 8080));
  SetUserAgent(kFetcherUserAgent);
  EXPECT_FALSE(ApacheTreatSelfFetchAsRemote(&request_));
  ExpectUntouched(peer);
}

}  // namespace
}  // namespace net_instaweb

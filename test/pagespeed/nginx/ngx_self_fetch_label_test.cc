// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The module's own fetches present as 224.0.0.0 to nginx's access rules: the
// peer test that recognizes them, the relabel of a connection's client
// address and its restore, on a stand-in with nginx's field names; plus a
// source pin that the module registers the relabel in the POST_READ phase
// and recognizes a self-fetch by marker and peer (the port cannot be linked
// into a test).

#include "pagespeed/nginx/ngx_self_fetch_label.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/loopback_route_fetcher.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// nginx's ngx_str_t and the ngx_connection_t fields the relabel touches.
struct FakeStr {
  size_t len;
  unsigned char* data;
};

struct FakeConnection {
  struct sockaddr* sockaddr;
  socklen_t socklen;
  FakeStr addr_text;
};

struct sockaddr_in V4(const char* ip, int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  EXPECT_EQ(1, inet_pton(AF_INET, ip, &a.sin_addr)) << ip;
  return a;
}

struct sockaddr_in6
V6(const char* ip, int port) {
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof(a));
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(port);
  EXPECT_EQ(1, inet_pton(AF_INET6, ip, &a.sin6_addr)) << ip;
  return a;
}

const struct sockaddr*
Sa(const struct sockaddr_in& a) {
  return reinterpret_cast<const struct sockaddr*>(&a);
}

const struct sockaddr* Sa(const struct sockaddr_in6& a) {
  return reinterpret_cast<const struct sockaddr*>(&a);
}

TEST(NgxSelfFetchLabelTest, LoopbackPeersAreThisServer) {
  struct sockaddr_in local = V4("192.0.2.20", 8080);
  EXPECT_TRUE(
      NgxSelfFetchPeerIsThisServer(Sa(V4("127.0.0.1", 40001)), Sa(local)));
  EXPECT_TRUE(
      NgxSelfFetchPeerIsThisServer(Sa(V4("127.9.8.7", 40001)), Sa(local)));
  EXPECT_TRUE(
      NgxSelfFetchPeerIsThisServer(Sa(V4("127.0.0.1", 40001)), nullptr));
  EXPECT_TRUE(NgxSelfFetchPeerIsThisServer(Sa(V6("::1", 40001)), nullptr));
}

TEST(NgxSelfFetchLabelTest, PeerOnTheArrivalAddressIsThisServer) {
  EXPECT_TRUE(NgxSelfFetchPeerIsThisServer(Sa(V4("192.0.2.20", 40002)),
                                           Sa(V4("192.0.2.20", 8080))));
  EXPECT_TRUE(NgxSelfFetchPeerIsThisServer(Sa(V6("2001:db8::20", 40002)),
                                           Sa(V6("2001:db8::20", 8080))));
}

TEST(NgxSelfFetchLabelTest, OtherPeersAreNotThisServer) {
  struct sockaddr_in local = V4("192.0.2.20", 8080);
  EXPECT_FALSE(
      NgxSelfFetchPeerIsThisServer(Sa(V4("192.0.2.21", 40003)), Sa(local)));
  EXPECT_FALSE(
      NgxSelfFetchPeerIsThisServer(Sa(V4("198.51.100.9", 40003)), Sa(local)));
  EXPECT_FALSE(
      NgxSelfFetchPeerIsThisServer(Sa(V4("198.51.100.9", 40003)), nullptr));
  EXPECT_FALSE(NgxSelfFetchPeerIsThisServer(Sa(V6("2001:db8::21", 40003)),
                                            Sa(V6("2001:db8::20", 8080))));
  // Different families never compare equal.
  EXPECT_FALSE(
      NgxSelfFetchPeerIsThisServer(Sa(V6("2001:db8::20", 40003)), Sa(local)));
  EXPECT_FALSE(NgxSelfFetchPeerIsThisServer(nullptr, Sa(local)));
}

TEST(NgxSelfFetchLabelTest, RelabelPresentsTheReservedAddress) {
  struct sockaddr_in peer = V4("127.0.0.1", 40004);
  struct sockaddr_in local = V4("127.0.0.1", 8080);
  unsigned char peer_text[] = "127.0.0.1";
  FakeConnection c;
  c.sockaddr = reinterpret_cast<struct sockaddr*>(&peer);
  c.socklen = sizeof(peer);
  c.addr_text.len = sizeof(peer_text) - 1;
  c.addr_text.data = peer_text;

  NgxSelfFetchLabel label;
  NgxSelfFetchSaved<FakeConnection> saved;
  NgxRelabelSelfFetch(&c, &label, &saved);

  ASSERT_EQ(AF_INET, c.sockaddr->sa_family);
  EXPECT_EQ(sizeof(struct sockaddr_in), static_cast<size_t>(c.socklen));
  const auto* now = reinterpret_cast<const struct sockaddr_in*>(c.sockaddr);
  char text[INET_ADDRSTRLEN];
  ASSERT_NE(nullptr, inet_ntop(AF_INET, &now->sin_addr, text, sizeof(text)));
  EXPECT_STREQ("224.0.0.0", text);
  EXPECT_EQ("224.0.0.0", std::string(reinterpret_cast<char*>(c.addr_text.data),
                                     c.addr_text.len));
  EXPECT_STREQ(kNgxSelfFetchLabelText, "224.0.0.0");
  // Neither loopback nor the server's own address any more.
  EXPECT_FALSE(LoopbackRouteFetcher::IsLoopbackAddr(c.sockaddr));
  EXPECT_FALSE(NgxSelfFetchPeerIsThisServer(c.sockaddr, Sa(local)));
}

TEST(NgxSelfFetchLabelTest, RestorePutsTheOriginalAddressBack) {
  struct sockaddr_in6 peer = V6("2001:db8::30", 40005);
  unsigned char peer_text[] = "2001:db8::30";
  FakeConnection c;
  c.sockaddr = reinterpret_cast<struct sockaddr*>(&peer);
  c.socklen = sizeof(peer);
  c.addr_text.len = sizeof(peer_text) - 1;
  c.addr_text.data = peer_text;

  NgxSelfFetchLabel label;
  NgxSelfFetchSaved<FakeConnection> saved;
  NgxRelabelSelfFetch(&c, &label, &saved);
  EXPECT_NE(reinterpret_cast<struct sockaddr*>(&peer), c.sockaddr);

  NgxRestoreSelfFetch(saved);
  EXPECT_EQ(reinterpret_cast<struct sockaddr*>(&peer), c.sockaddr);
  EXPECT_EQ(sizeof(peer), static_cast<size_t>(c.socklen));
  EXPECT_EQ(peer_text, c.addr_text.data);
  EXPECT_EQ(sizeof(peer_text) - 1, c.addr_text.len);
}

TEST(NgxSelfFetchLabelTest, NestedRelabelsRestoreInReverseOrder) {
  // Pool cleanups run last-added first, as another module that rewrites the
  // client address after ours (realip) would stack its own on top.
  struct sockaddr_in peer = V4("127.0.0.1", 40006);
  unsigned char peer_text[] = "127.0.0.1";
  FakeConnection c;
  c.sockaddr = reinterpret_cast<struct sockaddr*>(&peer);
  c.socklen = sizeof(peer);
  c.addr_text.len = sizeof(peer_text) - 1;
  c.addr_text.data = peer_text;

  NgxSelfFetchLabel first_label, second_label;
  NgxSelfFetchSaved<FakeConnection> first, second;
  NgxRelabelSelfFetch(&c, &first_label, &first);
  struct sockaddr* after_first = c.sockaddr;
  NgxRelabelSelfFetch(&c, &second_label, &second);
  NgxRestoreSelfFetch(second);
  EXPECT_EQ(after_first, c.sockaddr);
  NgxRestoreSelfFetch(first);
  EXPECT_EQ(reinterpret_cast<struct sockaddr*>(&peer), c.sockaddr);
  EXPECT_EQ(peer_text, c.addr_text.data);
}

// --- Wiring pin -----------------------------------------------------------
//
// The port cannot be linked into a unit test (see test/pagespeed/nginx/BUILD),
// so the registration and the handler's recognition are pinned by reading
// the source, with whitespace stripped.

GoogleString ReadStrippedModuleSource() {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/nginx/ngx_pagespeed.cc");
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(NgxSelfFetchLabelTest, ModuleRelabelsInPostReadByMarkerAndPeer) {
  const GoogleString src = ReadStrippedModuleSource();
  ASSERT_FALSE(src.empty());
  EXPECT_NE(GoogleString::npos,
            src.find("ngx_array_push(&cmcf->phases[NGX_HTTP_POST_READ_PHASE]"
                     ".handlers));if(sf_h==nullptr){returnNGX_ERROR;}"
                     "*sf_h=ps_self_fetch_post_read_handler;"));
  const size_t handler = src.find("ngx_int_tps_self_fetch_post_read_handler(");
  ASSERT_NE(GoogleString::npos, handler);
  const size_t end =
      src.find("returnNGX_DECLINED;}//preaccess_handler", handler);
  const GoogleString body = src.substr(
      handler, end == GoogleString::npos ? GoogleString::npos : end - handler);
  EXPECT_NE(GoogleString::npos, body.find("!is_pagespeed_subrequest(r)"));
  EXPECT_NE(GoogleString::npos,
            body.find("if(!NgxSelfFetchPeerIsThisServer(c->sockaddr,local))"));
  EXPECT_NE(GoogleString::npos, body.find("ngx_pool_cleanup_add(r->pool,"));
  EXPECT_NE(GoogleString::npos,
            body.find("NgxRelabelSelfFetch(c,&relabel->label,&relabel->saved);"
                      "cln->handler=ps_self_fetch_restore;"));
}

}  // namespace
}  // namespace net_instaweb

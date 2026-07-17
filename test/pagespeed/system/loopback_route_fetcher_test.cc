/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

//
// Unit tests for LoopbackRouteFetcher
//
#include "pagespeed/system/loopback_route_fetcher.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <memory>

#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/config/rewrite_options_manager.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/ref_counted_ptr.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/net/instaweb/http/mock_callback.h"
#include "test/net/instaweb/http/reflecting_test_fetcher.h"
#include "test/net/instaweb/rewriter/rewrite_options_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kOwnIp[] = "198.51.100.1";

// RAII wrapper for addrinfo results from getaddrinfo.
struct AddrInfoDeleter {
  void operator()(struct addrinfo* ai) const {
    if (ai != nullptr) {
      freeaddrinfo(ai);
    }
  }
};
using AddrInfoPtr = std::unique_ptr<struct addrinfo, AddrInfoDeleter>;

// Helper to resolve an IP address string to a sockaddr using getaddrinfo.
// Returns the addrinfo (caller owns via unique_ptr), or nullptr on failure.
AddrInfoPtr ResolveAddr(const char* ip, int family) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = family;
  hints.ai_flags = AI_NUMERICHOST;
  struct addrinfo* result = nullptr;
  int err = getaddrinfo(ip, "80", &hints, &result);
  if (err != 0) {
    return nullptr;
  }
  return AddrInfoPtr(result);
}

class LoopbackRouteFetcherTest : public RewriteOptionsTestBase<RewriteOptions> {
 public:
  LoopbackRouteFetcherTest()
      : thread_system_(Platform::CreateThreadSystem()),
        options_(thread_system_.get()),
        loopback_route_fetcher_(&options_, kOwnIp, 42, "",
                                &reflecting_fetcher_),
        plain_http_fetcher_(&options_, kOwnIp, 8080, "http",
                            &reflecting_fetcher_),
        tls_fetcher_(&options_, kOwnIp, 8443, "https", &reflecting_fetcher_),
        tls_default_port_fetcher_(&options_, kOwnIp, 443, "https",
                                  &reflecting_fetcher_) {}

  void PrepareDone(bool ok) { EXPECT_TRUE(ok); }

 protected:
  GoogleMessageHandler handler_;
  ReflectingTestFetcher reflecting_fetcher_;
  std::unique_ptr<ThreadSystem> thread_system_;
  RewriteOptions options_;
  // No connection scheme plumbed — legacy behavior.
  LoopbackRouteFetcher loopback_route_fetcher_;
  // Connection came in over plain http on port 8080.
  LoopbackRouteFetcher plain_http_fetcher_;
  // Connection came in over TLS on port 8443.
  LoopbackRouteFetcher tls_fetcher_;
  // Connection came in over TLS on port 443.
  LoopbackRouteFetcher tls_default_port_fetcher_;
};

TEST_F(LoopbackRouteFetcherTest, LoopbackRouteFetcherWorks) {
  // As we use the reflecting fetcher as the backend here, the reply
  // messages will contain the URL the fetcher got as payload. Further,
  // the reflecting fetcher will copy all the request headers it got into its
  // response's header, so we can use the result's response_headers to check
  // the request headers we sent.

  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  loopback_route_fetcher_.Fetch("http://somehost.com/url", &handler_, &dest);
  EXPECT_STREQ(StrCat("http://", kOwnIp, ":42/url"), dest.buffer());
  EXPECT_STREQ("somehost.com", dest.response_headers()->Lookup1("Host"));

  // And also test handling of protocol-relative urls.
  ExpectStringAsyncFetch destPR(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  loopback_route_fetcher_.Fetch("http://somehost.com//foo/bar", &handler_,
                                &destPR);
  EXPECT_STREQ(StrCat("http://", kOwnIp, ":42//foo/bar"), destPR.buffer());
  EXPECT_STREQ("somehost.com", destPR.response_headers()->Lookup1("Host"));

  // Now make somehost.com known, as well as somehost.cdn.com
  options_.WriteableDomainLawyer()->AddOriginDomainMapping(
      "somehost.cdn.com", "somehost.com", "", &handler_);

  ExpectStringAsyncFetch dest2(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  loopback_route_fetcher_.Fetch("http://somehost.com/url", &handler_, &dest2);
  EXPECT_STREQ("http://somehost.com/url", dest2.buffer());

  ExpectStringAsyncFetch dest3(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  loopback_route_fetcher_.Fetch("http://somehost.cdn.com/url", &handler_,
                                &dest3);
  EXPECT_STREQ("http://somehost.cdn.com/url", dest3.buffer());

  // Should still be redirected if the port doesn't match.
  ExpectStringAsyncFetch dest4(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  loopback_route_fetcher_.Fetch("http://somehost.cdn.com:123/url", &handler_,
                                &dest4);
  EXPECT_STREQ(StrCat("http://", kOwnIp, ":42/url"), dest4.buffer());
  EXPECT_STREQ("somehost.cdn.com:123",
               dest4.response_headers()->Lookup1("Host"));

  // Now add a session authorization for the CDN's origin. It should now
  // connect directly.
  RequestContextPtr request_context5(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  request_context5->AddSessionAuthorizedFetchOrigin(
      "http://somehost.cdn.com:123");

  ExpectStringAsyncFetch dest5(true, request_context5);
  loopback_route_fetcher_.Fetch("http://somehost.cdn.com:123/url", &handler_,
                                &dest5);
  EXPECT_STREQ("http://somehost.cdn.com:123/url", dest5.buffer());

  // The same authorization doesn't permit a different port, however.
  RequestContextPtr request_context6(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  request_context5->AddSessionAuthorizedFetchOrigin(
      "http://somehost.cdn.com:123");

  ExpectStringAsyncFetch dest6(true, request_context6);
  loopback_route_fetcher_.Fetch("http://somehost.cdn.com:456/url", &handler_,
                                &dest6);
  EXPECT_STREQ(StrCat("http://", kOwnIp, ":42/url"), dest6.buffer());
  EXPECT_STREQ("somehost.cdn.com:456",
               dest6.response_headers()->Lookup1("Host"));
}

// defect B: when X-Forwarded-Proto is in play the resource URL's
// scheme reflects the original client connection, not the transport this
// server actually speaks on own_port. Munging must use the connection's
// transport scheme, otherwise we synthesize structurally unfetchable URLs
// like https://127.0.0.1:<plain-http-port>/... — a guaranteed TLS handshake
// failure whose result is then remembered by the 300s failure cache.
TEST_F(LoopbackRouteFetcherTest, MungesWithConnectionSchemeOverPlainHttp) {
  // https resource URL (from an X-Forwarded-Proto: https page) arriving over
  // the plain-http listener on 8080 must loop back over plain http.
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  plain_http_fetcher_.Fetch("https://somehost.com/style.css", &handler_,
                            &dest);
  EXPECT_STREQ(StrCat("http://", kOwnIp, ":8080/style.css"), dest.buffer());
  // The Host header still carries the original authority, so cache keys and
  // virtual-host routing are unaffected.
  EXPECT_STREQ("somehost.com", dest.response_headers()->Lookup1("Host"));
}

TEST_F(LoopbackRouteFetcherTest, MungesWithConnectionSchemeOverTls) {
  // Mirror image: http resource URL arriving over a TLS listener loops back
  // over TLS.
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  tls_fetcher_.Fetch("http://somehost.com/app.js", &handler_, &dest);
  EXPECT_STREQ(StrCat("https://", kOwnIp, ":8443/app.js"), dest.buffer());
  EXPECT_STREQ("somehost.com", dest.response_headers()->Lookup1("Host"));
}

TEST_F(LoopbackRouteFetcherTest, ElidesDefaultPortOfConnectionScheme) {
  // Port elision must follow the connection scheme too: 443 is default for
  // the https transport even when the resource URL says http.
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  tls_default_port_fetcher_.Fetch("http://somehost.com/app.js", &handler_,
                                  &dest);
  EXPECT_STREQ(StrCat("https://", kOwnIp, "/app.js"), dest.buffer());
  EXPECT_STREQ("somehost.com", dest.response_headers()->Lookup1("Host"));
}

TEST_F(LoopbackRouteFetcherTest, EmptyConnectionSchemeKeepsResourceScheme) {
  // Ports that don't plumb the connection scheme keep the legacy behavior:
  // the resource URL's scheme survives into the munged URL.
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  loopback_route_fetcher_.Fetch("https://somehost.com/style.css", &handler_,
                                &dest);
  EXPECT_STREQ(StrCat("https://", kOwnIp, ":42/style.css"), dest.buffer());
  EXPECT_STREQ("somehost.com", dest.response_headers()->Lookup1("Host"));
}

TEST_F(LoopbackRouteFetcherTest, ConnectionSchemeDoesNotAffectKnownOrigins) {
  // Known origins are still fetched exactly as given (same mapping as in
  // LoopbackRouteFetcherWorks above).
  options_.WriteableDomainLawyer()->AddOriginDomainMapping(
      "somehost.cdn.com", "somehost.com", "", &handler_);
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  plain_http_fetcher_.Fetch("http://somehost.com/url", &handler_, &dest);
  EXPECT_STREQ("http://somehost.com/url", dest.buffer());
}

TEST_F(LoopbackRouteFetcherTest, CanDetectSelfSrc) {
  AddrInfoPtr loopback_1 = ResolveAddr("127.0.0.1", AF_INET);
  ASSERT_NE(nullptr, loopback_1.get());

  AddrInfoPtr loopback_2 = ResolveAddr("127.12.34.45", AF_INET);
  ASSERT_NE(nullptr, loopback_2.get());

  AddrInfoPtr loopback_3 = ResolveAddr("::1", AF_INET6);
  ASSERT_NE(nullptr, loopback_3.get());

  AddrInfoPtr loopback_4 = ResolveAddr("::FFFF:127.0.0.2", AF_INET6);
  ASSERT_NE(nullptr, loopback_4.get());

  AddrInfoPtr not_loopback_1 = ResolveAddr("128.0.0.1", AF_INET);
  ASSERT_NE(nullptr, not_loopback_1.get());

  AddrInfoPtr not_loopback_2 = ResolveAddr("::1:1", AF_INET6);
  ASSERT_NE(nullptr, not_loopback_2.get());

  AddrInfoPtr not_loopback_3 = ResolveAddr("::1:FFFF:127.0.0.1", AF_INET6);
  ASSERT_NE(nullptr, not_loopback_3.get());

  EXPECT_TRUE(
      LoopbackRouteFetcher::IsLoopbackAddr(loopback_1->ai_addr));
  EXPECT_TRUE(
      LoopbackRouteFetcher::IsLoopbackAddr(loopback_2->ai_addr));
  EXPECT_TRUE(
      LoopbackRouteFetcher::IsLoopbackAddr(loopback_3->ai_addr));
  EXPECT_TRUE(
      LoopbackRouteFetcher::IsLoopbackAddr(loopback_4->ai_addr));
  EXPECT_FALSE(
      LoopbackRouteFetcher::IsLoopbackAddr(not_loopback_1->ai_addr));
  EXPECT_FALSE(
      LoopbackRouteFetcher::IsLoopbackAddr(not_loopback_2->ai_addr));
  EXPECT_FALSE(
      LoopbackRouteFetcher::IsLoopbackAddr(not_loopback_3->ai_addr));
}

TEST_F(LoopbackRouteFetcherTest, ProxySuffix) {
  RewriteOptionsManager options_manager;
  DomainLawyer* lawyer = options_.WriteableDomainLawyer();
  lawyer->set_proxy_suffix(".suffix");

  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));

  GoogleString url("http://www.foo.com.suffix");
  RequestHeaders request_headers;
  options_manager.PrepareRequest(
      &options_, dest.request_context(), &url, &request_headers,
      NewCallback(this, &LoopbackRouteFetcherTest::PrepareDone));
  loopback_route_fetcher_.Fetch("http://www.foo.com", &handler_, &dest);
  EXPECT_STREQ("http://www.foo.com", dest.buffer());
  EXPECT_EQ(nullptr, dest.response_headers()->Lookup1("Host"));
}

}  // namespace

}  // namespace net_instaweb

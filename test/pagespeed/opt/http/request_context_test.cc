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

// Unit tests for RequestContext.

#include <memory>
#include <thread>

#include "pagespeed/opt/http/request_context.h"

#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class RequestContextTest : public testing::Test {
 public:
  RequestContextTest() : thread_system_(Platform::CreateThreadSystem()) {}

  RequestContextPtr MakeRequestContext() {
    return RequestContext::NewTestRequestContext(thread_system_.get());
  }

 private:
  std::unique_ptr<ThreadSystem> thread_system_;
  RequestContextTest(const RequestContextTest&) = delete;
  RequestContextTest& operator=(const RequestContextTest&) = delete;
};

TEST_F(RequestContextTest, ViaHttp2) {
  {
    RequestContextPtr rc(MakeRequestContext());
    rc->SetHttp2SupportFromViaHeader(" 2  coolproxy");
    EXPECT_TRUE(rc->using_http2());
  }

  {
    RequestContextPtr rc(MakeRequestContext());
    rc->SetHttp2SupportFromViaHeader("\t 2\t  coolproxy");
    EXPECT_TRUE(rc->using_http2());
  }

  {
    RequestContextPtr rc(MakeRequestContext());
    rc->SetHttp2SupportFromViaHeader("");
    EXPECT_FALSE(rc->using_http2());
  }

  // In case of multiple proxies, we look at the one closest to the user right
  // now --- though it's not always clear what the right answer is when many
  // proxies are involved, since we don't know how far they are from the user
  // and the server
  {
    RequestContextPtr rc(MakeRequestContext());
    rc->SetHttp2SupportFromViaHeader("http/2 coolproxy,1.1 someproxy");
    EXPECT_TRUE(rc->using_http2());
  }

  {
    RequestContextPtr rc(MakeRequestContext());
    rc->SetHttp2SupportFromViaHeader("weirdo/2 coolproxy,1.1 someproxy");
    EXPECT_FALSE(rc->using_http2());
  }

  {
    RequestContextPtr rc(MakeRequestContext());
    rc->SetHttp2SupportFromViaHeader("1.1 someproxy, 2 coolproxy");
    EXPECT_FALSE(rc->using_http2());
  }
}

// Parallel resource fetches for one page share a RequestContext: some threads
// authorize origins while others check them.
TEST_F(RequestContextTest, SessionAuthorizedFetchOriginsFromManyThreads) {
  RequestContextPtr rc(MakeRequestContext());
  const int kOrigins = 200;
  std::thread writer([&rc]() {
    for (int i = 0; i < kOrigins; ++i) {
      rc->AddSessionAuthorizedFetchOrigin(
          StrCat("http://o", IntegerToString(i), ".example.com"));
    }
  });
  std::thread reader([&rc]() {
    for (int i = 0; i < kOrigins; ++i) {
      rc->IsSessionAuthorizedFetchOrigin(
          StrCat("http://o", IntegerToString(i), ".example.com"));
    }
  });
  writer.join();
  reader.join();
  for (int i = 0; i < kOrigins; ++i) {
    EXPECT_TRUE(rc->IsSessionAuthorizedFetchOrigin(
        StrCat("http://o", IntegerToString(i), ".example.com")));
  }
  EXPECT_FALSE(rc->IsSessionAuthorizedFetchOrigin("http://other.example.com"));
}

}  // namespace net_instaweb

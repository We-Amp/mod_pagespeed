// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_caching_header_policy.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

NgxCachingHeaderRequest Html() { return NgxCachingHeaderRequest(); }

NgxCachingHeaderRequest Resource() {
  NgxCachingHeaderRequest request;
  request.pagespeed_resource = true;
  return request;
}

TEST(NgxCachingHeaderPolicyTest, ModifyCachingHeadersOffKeepsAPagesHeaders) {
  NgxCachingHeaderRequest request = Html();
  request.modify_caching_headers = false;
  EXPECT_EQ(kPreserveAllCachingHeaders, NgxPreserveCachingHeadersFor(request));
  request.downstream_cache_integration = true;
  EXPECT_EQ(kPreserveAllCachingHeaders, NgxPreserveCachingHeadersFor(request));
}

TEST(NgxCachingHeaderPolicyTest,
     ModifyCachingHeadersOffLeavesAResourceTheModulesHeaders) {
  NgxCachingHeaderRequest request = Resource();
  request.modify_caching_headers = false;
  EXPECT_EQ(kDontPreserveHeaders, NgxPreserveCachingHeadersFor(request));
  request.downstream_cache_integration = true;
  EXPECT_EQ(kDontPreserveHeaders, NgxPreserveCachingHeadersFor(request));
}

TEST(NgxCachingHeaderPolicyTest, AResourceGetsTheSameAnswerEitherWay) {
  for (bool downstream : {false, true}) {
    NgxCachingHeaderRequest on = Resource();
    on.downstream_cache_integration = downstream;
    NgxCachingHeaderRequest off = on;
    off.modify_caching_headers = false;
    EXPECT_EQ(NgxPreserveCachingHeadersFor(on),
              NgxPreserveCachingHeadersFor(off))
        << "downstream cache integration " << downstream;
  }
}

TEST(NgxCachingHeaderPolicyTest, ModifyCachingHeadersOnReplacesAPagesHeaders) {
  EXPECT_EQ(kDontPreserveHeaders, NgxPreserveCachingHeadersFor(Html()));
}

TEST(NgxCachingHeaderPolicyTest, DownstreamCachingKeepsAPagesCacheControl) {
  NgxCachingHeaderRequest request = Html();
  request.downstream_cache_integration = true;
  EXPECT_EQ(kPreserveOnlyCacheControl, NgxPreserveCachingHeadersFor(request));
  request.rebeaconing_key_matches = true;
  EXPECT_EQ(kDontPreserveHeaders, NgxPreserveCachingHeadersFor(request));
}

TEST(NgxCachingHeaderPolicyTest, DownstreamCachingLeavesHandlersTheirHeaders) {
  NgxCachingHeaderRequest request = Html();
  request.downstream_cache_integration = true;
  request.admin_handler = true;
  EXPECT_EQ(kDontPreserveHeaders, NgxPreserveCachingHeadersFor(request));
  request = Resource();
  request.downstream_cache_integration = true;
  EXPECT_EQ(kDontPreserveHeaders, NgxPreserveCachingHeadersFor(request));
}

}  // namespace
}  // namespace net_instaweb

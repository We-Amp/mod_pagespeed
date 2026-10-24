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

#include "pagespeed/kernel/http/caching_headers.h"

#include <memory>

#include "pagespeed/kernel/http/http_names.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class TestCachingHeaders : public CachingHeaders {
 public:
  explicit TestCachingHeaders(const char* cache_control)
      : CachingHeaders(HttpStatus::kOK),
        cache_control_(cache_control),
        likely_static_resource_type_(true),
        cacheable_resource_status_code_(true) {}

  bool Lookup(const StringPiece& key, StringPieceVector* values) override {
    if (key == HttpAttributes::kCacheControl) {
      SplitStringPieceToVector(cache_control_, ",", values, true);
      for (auto& value : *values) {
        TrimWhitespace(&value);
      }
      return true;
    } else if (key == HttpAttributes::kPragma && !pragma_.empty()) {
      values->push_back(pragma_);
      return true;
    } else {
      return false;
    }
  }

  bool IsLikelyStaticResourceType() const override {
    return likely_static_resource_type_;
  }

  bool IsCacheableResourceStatusCode() const override {
    return cacheable_resource_status_code_;
  }

  void set_likely_static_resource_type(bool x) {
    likely_static_resource_type_ = x;
  }

  void set_cacheable_resource_status_code(bool x) {
    cacheable_resource_status_code_ = x;
  }

  void set_pragma(const char* pragma) { pragma_ = pragma; }

 private:
  GoogleString cache_control_;
  GoogleString pragma_;
  bool likely_static_resource_type_;
  bool cacheable_resource_status_code_;

  TestCachingHeaders(const TestCachingHeaders&) = delete;
  TestCachingHeaders& operator=(const TestCachingHeaders&) = delete;
};

class CachingHeadersTest : public testing::Test {
 protected:
  void SetCacheControl(const char* cache_control) {
    headers_ = std::make_unique<TestCachingHeaders>(cache_control);
  }

  GoogleString DisableCacheControl() {
    return headers_->GenerateDisabledCacheControl();
  }

  std::unique_ptr<TestCachingHeaders> headers_;
};

TEST_F(CachingHeadersTest, DisableEmpty) {
  SetCacheControl("");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());
}

TEST_F(CachingHeadersTest, DisableCaching) {
  SetCacheControl("max-age=60");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());
}

TEST_F(CachingHeadersTest, DisablePrivateCaching) {
  SetCacheControl("private, max-age=60");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());
}

TEST_F(CachingHeadersTest, DisablePublicCaching) {
  SetCacheControl("public, max-age=60");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());
}

TEST_F(CachingHeadersTest, DisableNostore) {
  SetCacheControl("must-revalidate, private, no-store");
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               DisableCacheControl());
}

TEST_F(CachingHeadersTest, DisableNostoreRetainNoCache) {
  SetCacheControl("no-cache, must-revalidate, private, no-store");
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               DisableCacheControl());
}

TEST_F(CachingHeadersTest, DisableSMaxAge) {
  // s-maxage overrides max-age for shared caches, so letting it survive
  // would invite a shared cache to keep serving a response whose caching we
  // are disabling (rewritten HTML once went out as
  // "max-age=0, no-cache, s-maxage=10").
  SetCacheControl("s-maxage=10");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());

  SetCacheControl("max-age=60, s-maxage=10");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());

  SetCacheControl("public, max-age=300, s-maxage=3600");
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, DisableCacheControl());

  // Other directives around a dropped s-maxage are still preserved.
  SetCacheControl("no-cache, s-maxage=600, no-store");
  EXPECT_STREQ(
      StrCat(HttpAttributes::kNoCacheMaxAge0, ", ", HttpAttributes::kNoStore),
      DisableCacheControl());
}

TEST_F(CachingHeadersTest, IsCacheable) {
  // Default of no headers, likely static resource type and cacheable status
  // code is cacheable.
  SetCacheControl("");
  EXPECT_TRUE(headers_->IsCacheable());

  // It's false if type isn't likely static or status isn't cacheable, though.
  SetCacheControl("");
  headers_->set_cacheable_resource_status_code(false);
  EXPECT_FALSE(headers_->IsCacheable());

  SetCacheControl("");
  headers_->set_likely_static_resource_type(false);
  EXPECT_FALSE(headers_->IsCacheable());

  // Private is OK, for browser cacheability.
  SetCacheControl("private");
  EXPECT_TRUE(headers_->IsCacheable());
  EXPECT_FALSE(headers_->IsProxyCacheable());

  // Various flags that make it non-cacheable.
  SetCacheControl("no-cache");
  EXPECT_FALSE(headers_->IsCacheable());

  SetCacheControl("no-store");
  EXPECT_FALSE(headers_->IsCacheable());

  SetCacheControl("must-revalidate");
  EXPECT_FALSE(headers_->IsCacheable());
  EXPECT_FALSE(headers_->ProxyRevalidate());
  EXPECT_TRUE(headers_->MustRevalidate());
  EXPECT_FALSE(headers_->IsProxyCacheable());

  SetCacheControl("proxy-revalidate");
  EXPECT_TRUE(headers_->IsCacheable());
  EXPECT_TRUE(headers_->ProxyRevalidate());
  EXPECT_FALSE(headers_->MustRevalidate());

  // must-revalidate does not imply uncacheability: it just means
  // that stale content should not be trusted.
  SetCacheControl("must-revalidate,max-age=600");
  EXPECT_FALSE(headers_->ProxyRevalidate());
  EXPECT_TRUE(headers_->MustRevalidate());
  EXPECT_TRUE(headers_->IsProxyCacheable());

  // proxy-revalidate is similar, but does not affect browser heuristics
  SetCacheControl("proxy-revalidate,max-age=600");
  EXPECT_TRUE(headers_->ProxyRevalidate());
  EXPECT_FALSE(headers_->MustRevalidate());
  EXPECT_TRUE(headers_->IsProxyCacheable());
}

TEST_F(CachingHeadersTest, CaseInsensitiveDirectives) {
  // Cache-Control directive names are case-insensitive per RFC 7234 5.2,
  // so mixed-case spellings must be honored exactly like lowercase ones.
  SetCacheControl("No-Store");
  EXPECT_FALSE(headers_->IsCacheable());
  EXPECT_TRUE(headers_->HasExplicitNoCacheDirective());

  SetCacheControl("NO-CACHE");
  EXPECT_FALSE(headers_->IsCacheable());

  SetCacheControl("Private");
  EXPECT_TRUE(headers_->IsCacheable());
  EXPECT_FALSE(headers_->IsProxyCacheable());

  SetCacheControl("MAX-AGE=0");
  EXPECT_FALSE(headers_->IsCacheable());
  EXPECT_TRUE(headers_->HasExplicitNoCacheDirective());

  SetCacheControl("Max-Age=600");
  EXPECT_TRUE(headers_->IsCacheable());
  EXPECT_TRUE(headers_->IsExplicitlyCacheable());

  SetCacheControl("MUST-REVALIDATE");
  EXPECT_TRUE(headers_->MustRevalidate());

  SetCacheControl("PROXY-REVALIDATE");
  EXPECT_TRUE(headers_->ProxyRevalidate());

  // Pragma: no-cache is case-insensitive too.
  SetCacheControl("");
  headers_->set_pragma("No-Cache");
  EXPECT_TRUE(headers_->HasExplicitNoCacheDirective());
}

TEST_F(CachingHeadersTest, SMaxAgeOnlyShortensFreshness) {
  // s-maxage is the shared-cache window (RFC 7234 4.2.1). It may shorten
  // the freshness that max-age or Expires grants, but must not lengthen
  // it: the computed lifetime also becomes the max-age handed to browsers,
  // which honor max-age and not s-maxage. A response that forbids
  // shared-cache storage via s-maxage=0 must not be held for max-age's
  // full year.
  SetCacheControl("max-age=31536000, s-maxage=0");
  EXPECT_TRUE(headers_->HasExplicitNoCacheDirective());
  EXPECT_FALSE(headers_->IsCacheable());
  EXPECT_FALSE(headers_->IsExplicitlyCacheable());
  int64 freshness_ms = -1;
  EXPECT_TRUE(headers_->GetFreshnessLifetimeMillis(&freshness_ms));
  EXPECT_EQ(0, freshness_ms);

  // A long s-maxage does not turn max-age=0 into a cacheable response:
  // the no-cache decision from max-age<=0 is never overridden.
  SetCacheControl("s-maxage=31536000, max-age=0");
  EXPECT_TRUE(headers_->HasExplicitNoCacheDirective());
  EXPECT_FALSE(headers_->IsCacheable());
  EXPECT_FALSE(headers_->IsExplicitlyCacheable());
  freshness_ms = -1;
  EXPECT_TRUE(headers_->GetFreshnessLifetimeMillis(&freshness_ms));
  EXPECT_EQ(0, freshness_ms);

  // A longer s-maxage does not extend the max-age lifetime.
  SetCacheControl("max-age=60, s-maxage=86400");
  EXPECT_TRUE(headers_->GetFreshnessLifetimeMillis(&freshness_ms));
  EXPECT_EQ(60000, freshness_ms);

  // A shorter one shortens it.
  SetCacheControl("max-age=86400, s-maxage=60");
  EXPECT_TRUE(headers_->GetFreshnessLifetimeMillis(&freshness_ms));
  EXPECT_EQ(60000, freshness_ms);

  // When both are positive and s-maxage is smaller, it wins on magnitude.
  SetCacheControl("max-age=60, s-maxage=30");
  EXPECT_TRUE(headers_->GetFreshnessLifetimeMillis(&freshness_ms));
  EXPECT_EQ(30000, freshness_ms);

  // Directive names are case-insensitive.
  SetCacheControl("MAX-AGE=60, S-MAXAGE=0");
  EXPECT_TRUE(headers_->HasExplicitNoCacheDirective());
}

TEST_F(CachingHeadersTest, SMaxAgeAloneDoesNotSetFreshness) {
  // s-maxage only shortens a lifetime that max-age or Expires grants. On
  // its own it must not create one: the computed lifetime becomes the
  // max-age browsers receive, and the origin gave browsers none.
  SetCacheControl("s-maxage=600");
  EXPECT_FALSE(headers_->HasExplicitFreshnessLifetime());
  EXPECT_FALSE(headers_->IsExplicitlyCacheable());
  int64 freshness_ms = -1;
  EXPECT_FALSE(headers_->GetFreshnessLifetimeMillis(&freshness_ms));
  EXPECT_EQ(0, freshness_ms);
}

}  // namespace net_instaweb

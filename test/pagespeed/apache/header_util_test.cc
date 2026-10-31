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

#include <memory>

#include "pagespeed/apache/header_util.h"

#include "http_request.h"  // NOLINT
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class HeaderUtilTest : public testing::Test {
 public:
  void PredicateMatchingA(StringPiece name, bool* ok) { *ok = (name == "a"); }

 protected:
  void SetUp() override {
    MockApache::Initialize();
    MockApache::PrepareRequest(&request_);
    // request_ starts as an uninitialized C struct: PrepareRequest fills the
    // pool and the tables it needs, and the two AddOriginContentEncoding
    // reads and the ApplyInPlaceSMaxAge read of user would otherwise be reads
    // of whatever was in the struct.
    request_.err_headers_out = apr_table_make(request_.pool, 10);
    request_.content_encoding = nullptr;
    request_.method = "GET";
    request_.user = nullptr;
  }

  void TearDown() override {
    MockApache::CleanupRequest(&request_);
    MockApache::Terminate();
  }

  void SetLastModified(const char* last_modified) {
    apr_table_set(request_.headers_out, HttpAttributes::kLastModified,
                  last_modified);
  }

  const char* GetLastModified() {
    return apr_table_get(request_.headers_out, HttpAttributes::kLastModified);
  }

  void SetCacheControl(const char* cache_control) {
    apr_table_set(request_.headers_out, HttpAttributes::kCacheControl,
                  cache_control);
  }

  const char* GetCacheControl() {
    return apr_table_get(request_.headers_out, HttpAttributes::kCacheControl);
  }

  void SetErrCacheControl(const char* cache_control) {
    apr_table_set(request_.err_headers_out, HttpAttributes::kCacheControl,
                  cache_control);
  }

  const char* GetErrCacheControl() {
    return apr_table_get(request_.err_headers_out,
                         HttpAttributes::kCacheControl);
  }

  request_rec request_;
};

TEST_F(HeaderUtilTest, DisableEmpty) {
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, GetCacheControl());
}

TEST_F(HeaderUtilTest, DisableCaching) {
  SetCacheControl("max-age=60");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, GetCacheControl());
}

TEST_F(HeaderUtilTest, DisablePrivateCaching) {
  SetCacheControl("private, max-age=60");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, GetCacheControl());
}

TEST_F(HeaderUtilTest, DisablePublicCaching) {
  SetCacheControl("public, max-age=60");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, GetCacheControl());
}

TEST_F(HeaderUtilTest, DisableNostore) {
  SetCacheControl("must-revalidate, private, no-store");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               GetCacheControl());
}

TEST_F(HeaderUtilTest, DisableNostoreRetainNoCache) {
  SetCacheControl("no-cache, must-revalidate, private, no-store");
  SetLastModified("some random string");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               GetCacheControl());
  EXPECT_STREQ("some random string", GetLastModified());
}

TEST_F(HeaderUtilTest, DisableCachingRelatedHeaders) {
  SetCacheControl("no-cache, must-revalidate, private, no-store");
  SetLastModified("some random string");
  DisableCachingRelatedHeaders(&request_);
  DisableCacheControlHeader(&request_);
  EXPECT_EQ(nullptr, GetLastModified());
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               GetCacheControl());
}

TEST_F(HeaderUtilTest, NoOriginEncodingAddsNothing) {
  ResponseHeaders headers;
  AddOriginContentEncoding(request_, &headers);
  EXPECT_TRUE(headers.Lookup1(HttpAttributes::kContentEncoding) == nullptr);
}

TEST_F(HeaderUtilTest, OriginEncodingFromErrHeadersOut) {
  // A script origin (CGI, FastCGI, PHP) writes its own headers, an encoding
  // included, into err_headers_out; before the protocol filters merge the
  // two tables, headers_out alone does not show it.
  apr_table_set(request_.err_headers_out, HttpAttributes::kContentEncoding,
                "gzip");
  ResponseHeaders headers;
  AddOriginContentEncoding(request_, &headers);
  EXPECT_STREQ("gzip", headers.Lookup1(HttpAttributes::kContentEncoding));
}

TEST_F(HeaderUtilTest, OriginEncodingFromModMime) {
  // mod_mime records the coding an AddEncoding extension stands for in
  // content_encoding (the pre-compressed-asset recipe, AddEncoding gzip
  // .gz), which the protocol filters merge into headers_out only later.
  apr_table_set(request_.err_headers_out, HttpAttributes::kContentEncoding,
                "");
  request_.content_encoding = "br";
  ResponseHeaders headers;
  AddOriginContentEncoding(request_, &headers);
  EXPECT_STREQ("br", headers.Lookup1(HttpAttributes::kContentEncoding));
}

TEST_F(HeaderUtilTest, AnEncodingInHeadersOutStands) {
  // Whatever headers_out already states is authoritative at this point (an
  // upstream's coding sits there), and the helper must not overwrite it
  // with a second value off the request.
  apr_table_set(request_.headers_out, HttpAttributes::kContentEncoding,
                "deflate");
  request_.content_encoding = "gzip";
  ResponseHeaders headers;
  ApacheRequestToResponseHeaders(request_, &headers, nullptr);
  AddOriginContentEncoding(request_, &headers);
  EXPECT_STREQ("deflate", headers.Lookup1(HttpAttributes::kContentEncoding));
}

// php-fpm, CGI scripts and "Header always set" deliver their caching
// headers in err_headers_out, which httpd merges with headers_out when
// sending the response.  The disable helpers must cover both tables, or the
// origin's values survive next to ours.
TEST_F(HeaderUtilTest, DisableErrCaching) {
  SetErrCacheControl("max-age=60");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, GetCacheControl());
  EXPECT_EQ(nullptr, GetErrCacheControl());
}

TEST_F(HeaderUtilTest, DisableErrNostore) {
  SetErrCacheControl("must-revalidate, private, no-store");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               GetCacheControl());
  EXPECT_EQ(nullptr, GetErrCacheControl());
}

TEST_F(HeaderUtilTest, DisableErrSMaxAge) {
  SetErrCacheControl("public, s-maxage=600");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(HttpAttributes::kNoCacheMaxAge0, GetCacheControl());
  EXPECT_EQ(nullptr, GetErrCacheControl());
}

TEST_F(HeaderUtilTest, DisableCachingInBothTables) {
  SetCacheControl("max-age=60");
  SetErrCacheControl("no-store");
  DisableCacheControlHeader(&request_);
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", ",
                      HttpAttributes::kNoStore),
               GetCacheControl());
  EXPECT_EQ(nullptr, GetErrCacheControl());
}

TEST_F(HeaderUtilTest, DisableCachingRelatedHeadersInErrHeaders) {
  SetCacheControl("no-cache, must-revalidate, private, no-store");
  SetLastModified("some random string");
  apr_table_set(request_.err_headers_out, HttpAttributes::kLastModified,
                "another random string");
  apr_table_set(request_.err_headers_out, HttpAttributes::kExpires,
                "sometime in the future");
  apr_table_set(request_.err_headers_out, HttpAttributes::kEtag, "\"v1\"");
  DisableCachingRelatedHeaders(&request_);
  DisableCacheControlHeader(&request_);
  EXPECT_EQ(nullptr, GetLastModified());
  EXPECT_EQ(nullptr, GetErrCacheControl());
  EXPECT_EQ(nullptr, apr_table_get(request_.err_headers_out,
                                   HttpAttributes::kLastModified));
  EXPECT_EQ(nullptr,
            apr_table_get(request_.err_headers_out, HttpAttributes::kExpires));
  EXPECT_EQ(nullptr,
            apr_table_get(request_.err_headers_out, HttpAttributes::kEtag));
  EXPECT_STREQ(StrCat(HttpAttributes::kNoCacheMaxAge0, ", must-revalidate, ",
                      HttpAttributes::kNoStore),
               GetCacheControl());
}

// In-place s-maxage must see Cache-Control in both header tables and leave a
// single merged header behind.
class InPlaceSMaxAgeTest : public HeaderUtilTest {
 protected:
  int CountCacheControl(apr_table_t* table) {
    int count = 0;
    apr_table_do(
        [](void* rec, const char*, const char*) {
          ++*static_cast<int*>(rec);
          return 1;
        },
        &count, table, HttpAttributes::kCacheControl, nullptr);
    return count;
  }
};

TEST_F(InPlaceSMaxAgeTest, OnlyInHeadersOut) {
  SetCacheControl("public, max-age=600");
  bool had = false;
  GoogleString original;
  ASSERT_TRUE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_TRUE(had);
  EXPECT_EQ("public, max-age=600", original);
  EXPECT_STREQ("public, max-age=600, s-maxage=10", GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.headers_out));
  EXPECT_EQ(0, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, OnlyInErrHeadersOut) {
  apr_table_set(request_.err_headers_out, HttpAttributes::kCacheControl,
                "public, max-age=600");
  bool had = false;
  GoogleString original;
  ASSERT_TRUE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_TRUE(had);
  EXPECT_EQ("public, max-age=600", original);
  EXPECT_STREQ("public, max-age=600, s-maxage=10", GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.headers_out));
  EXPECT_EQ(0, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, InBothTables) {
  SetCacheControl("public");
  apr_table_set(request_.err_headers_out, HttpAttributes::kCacheControl,
                "max-age=600");
  bool had = false;
  GoogleString original;
  ASSERT_TRUE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_TRUE(had);
  EXPECT_EQ("max-age=600, public", original);
  EXPECT_STREQ("max-age=600, public, s-maxage=10", GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.headers_out));
  EXPECT_EQ(0, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, TwoEntriesInHeadersOut) {
  apr_table_add(request_.headers_out, HttpAttributes::kCacheControl, "public");
  apr_table_add(request_.headers_out, HttpAttributes::kCacheControl,
                "max-age=600");
  bool had = false;
  GoogleString original;
  ASSERT_TRUE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_TRUE(had);
  EXPECT_EQ("public, max-age=600", original);
  EXPECT_STREQ("public, max-age=600, s-maxage=10", GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.headers_out));
  EXPECT_EQ(0, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, NoOriginalAddsSMaxAge) {
  bool had = true;
  GoogleString original;
  ASSERT_TRUE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_FALSE(had);
  EXPECT_STREQ("s-maxage=10", GetCacheControl());
}

TEST_F(InPlaceSMaxAgeTest, PrivateInErrHeadersOutLeavesTablesUntouched) {
  apr_table_set(request_.err_headers_out, HttpAttributes::kCacheControl,
                "private");
  bool had = false;
  GoogleString original;
  EXPECT_FALSE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_EQ(nullptr, GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, AuthorizedRequestIsNotStamped) {
  // s-maxage lets a shared cache store the response to a request that carried
  // Authorization and reuse it for other clients, so the origin's
  // Cache-Control goes out as sent.
  apr_table_set(request_.headers_in, HttpAttributes::kAuthorization,
                "Basic dXNlcjE6cGFzc3dvcmQ=");
  SetCacheControl("max-age=600");
  bool had = false;
  GoogleString original;
  EXPECT_FALSE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_STREQ("max-age=600", GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.headers_out));
  EXPECT_EQ(0, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, AuthorizedRequestWithoutCacheControlIsNotStamped) {
  // With no Cache-Control from the origin, the response to an authenticated
  // request stays without one rather than gaining s-maxage alone.
  apr_table_set(request_.headers_in, HttpAttributes::kAuthorization,
                "Bearer dXNlcjE6cGFzc3dvcmQ=");
  bool had = true;
  GoogleString original;
  EXPECT_FALSE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_EQ(nullptr, GetCacheControl());
  EXPECT_EQ(0, CountCacheControl(request_.err_headers_out));
}

TEST_F(InPlaceSMaxAgeTest, AuthenticatedUserIsNotStamped) {
  // Apache sets request->user for a request it authenticated by any scheme,
  // with or without an Authorization header; the recorder classifies that
  // request as authorized, and so does the stamp.
  static char kUser[] = "user1";
  request_.user = kUser;
  SetCacheControl("max-age=600");
  bool had = false;
  GoogleString original;
  EXPECT_FALSE(ApplyInPlaceSMaxAge(&request_, 10, &had, &original));
  EXPECT_STREQ("max-age=600", GetCacheControl());
  EXPECT_EQ(1, CountCacheControl(request_.headers_out));
}

TEST_F(HeaderUtilTest, SelectiveRequestHeaders) {
  apr_table_set(request_.headers_in, "a", "b");
  apr_table_set(request_.headers_in, "c", "d");
  RequestHeaders all, selective;
  ApacheRequestToRequestHeaders(request_, &all);
  EXPECT_STREQ("b", all.Lookup1("a"));
  EXPECT_STREQ("d", all.Lookup1("c"));
  EXPECT_EQ(2, all.NumAttributes());
  HeaderUtilTest* test = this;
  std::unique_ptr<HeaderPredicateFn> predicate(
      NewPermanentCallback(test, &HeaderUtilTest::PredicateMatchingA));
  ApacheRequestToRequestHeaders(request_, &selective, predicate.get());
  EXPECT_STREQ("b", selective.Lookup1("a"));
  EXPECT_TRUE(selective.Lookup1("c") == nullptr);
  EXPECT_EQ(1, selective.NumAttributes());
}

TEST_F(HeaderUtilTest, PropagatesRequestMethod) {
  const struct {
    const char* name;
    RequestHeaders::Method method;
  } kCases[] = {
      {"GET", RequestHeaders::kGet},   {"HEAD", RequestHeaders::kHead},
      {"POST", RequestHeaders::kPost}, {"PUT", RequestHeaders::kPut},
      {"DELETE", RequestHeaders::kDelete},
  };
  for (const auto& test_case : kCases) {
    request_.method = test_case.name;
    RequestHeaders headers;
    ApacheRequestToRequestHeaders(request_, &headers);
    EXPECT_EQ(test_case.method, headers.method()) << test_case.name;
  }
}

}  // namespace net_instaweb

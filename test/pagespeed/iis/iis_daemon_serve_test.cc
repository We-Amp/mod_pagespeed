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


// The serve arm's composition: what the module states on the wire for one
// decision, read as a list rather than as a sequence of writes into an IIS
// response. Every case here is a header field an operator or a cache can
// see, so each is pinned by name.

#include "pagespeed/iis/iis_daemon_serve.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/daemon_serve_arm.h"

namespace net_instaweb {

namespace {

const char kBody[] = "body { color: red }";

// A decision an optimized 200 would carry, so each case varies one thing.
DaemonServeDecision OptimizedHit() {
  DaemonServeDecision decision;
  decision.verdict = DaemonServeVerdict::kServeOptimized;
  decision.serve_class = kPsServeClassOptimized;
  decision.worker_processed = true;
  decision.origin_content_length = 100;
  decision.cache_control = "max-age=600, public";
  decision.etag = "W/\"PSA-abc\"";
  decision.content_type = "text/css";
  decision.age_seconds = 42;
  decision.origin_last_modified = 1600000000;
  decision.body = StringPiece(kBody, STATIC_STRLEN(kBody));
  return decision;
}

// The value of one header field, or the empty string when it is absent.
GoogleString Field(const IisServeResponse& response, StringPiece name) {
  for (size_t i = 0; i < response.headers.size(); ++i) {
    if (StringCaseEqual(response.headers[i].first, name)) {
      return response.headers[i].second;
    }
  }
  return "";
}

int FieldCount(const IisServeResponse& response, StringPiece name) {
  int n = 0;
  for (size_t i = 0; i < response.headers.size(); ++i) {
    if (StringCaseEqual(response.headers[i].first, name)) {
      ++n;
    }
  }
  return n;
}

TEST(IisDaemonServeTest, AnOptimizedHitStatesTheEntrysFields) {
  IisServeResponse response;
  ComposeIisServeResponse(OptimizedHit(), true, false, &response);

  EXPECT_EQ(200, response.status);
  EXPECT_EQ("max-age=600, public", Field(response, HttpAttributes::kCacheControl));
  EXPECT_EQ("W/\"PSA-abc\"", Field(response, HttpAttributes::kEtag));
  EXPECT_EQ("text/css", Field(response, HttpAttributes::kContentType));
  EXPECT_EQ("42", Field(response, HttpAttributes::kAge));
  EXPECT_TRUE(response.has_body);
  EXPECT_EQ(GoogleString(kBody), response.body.as_string());
  // The origin's validator belongs to origin bytes: an optimized variant is
  // not a representation the origin ever produced.
  EXPECT_EQ("", Field(response, HttpAttributes::kLastModified));
}

TEST(IisDaemonServeTest, AServedOriginalCarriesTheOriginsLastModified) {
  DaemonServeDecision decision = OptimizedHit();
  decision.verdict = DaemonServeVerdict::kServeOriginal;
  IisServeResponse response;
  ComposeIisServeResponse(decision, true, false, &response);

  EXPECT_NE("", Field(response, HttpAttributes::kLastModified));
}

TEST(IisDaemonServeTest, TheEncodingAxisIsStatedOnTheNotModifiedLegOnly) {
  IisServeResponse two_hundred;
  ComposeIisServeResponse(OptimizedHit(), true, false, &two_hundred);
  DaemonServeDecision not_modified = OptimizedHit();
  not_modified.not_modified = true;
  IisServeResponse three_oh_four;
  ComposeIisServeResponse(not_modified, true, false, &three_oh_four);

  // On the 200 the server's compressor is the emitter and appends its own
  // statement to anything written here; on the 304 there is no compressor,
  // so this is the only emitter. Where the server does not compress at all,
  // the 304 names an axis its 200 does not -- a wider key than the cache
  // had, which is the safe direction.
  EXPECT_EQ("", Field(two_hundred, HttpAttributes::kVary));
  EXPECT_EQ(1, FieldCount(three_oh_four, HttpAttributes::kVary));
  EXPECT_EQ(GoogleString(HttpAttributes::kAcceptEncoding),
            Field(three_oh_four, HttpAttributes::kVary));
}

TEST(IisDaemonServeTest, ANegotiatedEntryVariesOnAcceptOnBothLegs) {
  // `Accept` is the arm's own axis on either leg: no compressor states it.
  DaemonServeDecision decision = OptimizedHit();
  decision.emit_vary_accept = true;
  IisServeResponse two_hundred;
  ComposeIisServeResponse(decision, true, false, &two_hundred);
  decision.not_modified = true;
  IisServeResponse three_oh_four;
  ComposeIisServeResponse(decision, true, false, &three_oh_four);

  EXPECT_EQ("Accept", Field(two_hundred, HttpAttributes::kVary));
  EXPECT_EQ("Accept, Accept-Encoding",
            Field(three_oh_four, HttpAttributes::kVary));
}

TEST(IisDaemonServeTest, AnIncompressibleTypeDoesNotVaryOnTheEncoding) {
  DaemonServeDecision decision = OptimizedHit();
  decision.content_type = "image/jpeg";
  decision.not_modified = true;
  IisServeResponse response;
  ComposeIisServeResponse(decision, false, false, &response);

  EXPECT_EQ("", Field(response, HttpAttributes::kVary));
}

TEST(IisDaemonServeTest, ANotModifiedCarriesNoBodyAndNoContentType) {
  DaemonServeDecision decision = OptimizedHit();
  decision.not_modified = true;
  IisServeResponse response;
  ComposeIisServeResponse(decision, true, false, &response);

  EXPECT_EQ(304, response.status);
  EXPECT_FALSE(response.has_body);
  EXPECT_EQ("", Field(response, HttpAttributes::kContentType));
  // Age is stated on every hit, the 304 included: without it a cache keeps
  // the response for longer than the entry has left.
  EXPECT_EQ("42", Field(response, HttpAttributes::kAge));
  EXPECT_EQ("W/\"PSA-abc\"", Field(response, HttpAttributes::kEtag));
  EXPECT_EQ(GoogleString(HttpAttributes::kAcceptEncoding),
            Field(response, HttpAttributes::kVary));
}

TEST(IisDaemonServeTest, AHeadCarriesTheHeadersItsGetWouldAndNoBody) {
  IisServeResponse get;
  ComposeIisServeResponse(OptimizedHit(), true, false, &get);
  IisServeResponse head;
  ComposeIisServeResponse(OptimizedHit(), true, true, &head);

  EXPECT_FALSE(head.has_body);
  EXPECT_EQ(get.status, head.status);
  // The length its GET would return, so a client asking a HEAD gets the
  // answer it asked for.
  EXPECT_EQ(get.content_length, head.content_length);
  EXPECT_EQ(STATIC_STRLEN(kBody), head.content_length);
  ASSERT_EQ(get.headers.size(), head.headers.size());
  for (size_t i = 0; i < get.headers.size(); ++i) {
    EXPECT_EQ(get.headers[i].first, head.headers[i].first);
    EXPECT_EQ(get.headers[i].second, head.headers[i].second);
  }
}

TEST(IisDaemonServeTest, TheCompressiblePredicateLeavesRasterImagesAlone) {
  EXPECT_FALSE(IisCompressibleMediaType("image/jpeg"));
  EXPECT_FALSE(IisCompressibleMediaType("image/png"));
  EXPECT_FALSE(IisCompressibleMediaType("image/gif"));
  EXPECT_FALSE(IisCompressibleMediaType("image/webp"));
  EXPECT_FALSE(IisCompressibleMediaType("image/avif"));
  EXPECT_TRUE(IisCompressibleMediaType("text/css"));
  EXPECT_TRUE(IisCompressibleMediaType("application/javascript"));
  EXPECT_TRUE(IisCompressibleMediaType("image/svg+xml"));
  // Fail-open: an unknown type is treated as one IIS may compress.
  EXPECT_TRUE(IisCompressibleMediaType("application/x-newly-invented"));
  EXPECT_TRUE(IisCompressibleMediaType(""));
}

}  // namespace

}  // namespace net_instaweb

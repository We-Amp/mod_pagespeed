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

#include "pagespeed/nginx/ngx_daemon_serve_emit.h"

#include "base/logging.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class NgxDaemonServeEmitTest : public testing::Test {};

// A decision for an optimized hit with everything present: the composed
// response must pick each field up exactly once.  `body_bytes` sizes the
// representation the decision borrows; it must not exceed the static
// storage, which is checked rather than silently overrun.
DaemonServeDecision HitDecision(const char* content_type, bool not_modified,
                                size_t body_bytes) {
  DaemonServeDecision decision;
  decision.verdict = DaemonServeVerdict::kServeOptimized;
  decision.not_modified = not_modified;
  decision.content_type = content_type;
  decision.cache_control = "max-age=590, public";
  decision.etag = "W/\"ps-abc\"";
  decision.age_seconds = 11;
  decision.emit_vary_accept = true;
  decision.origin_last_modified = 0;
  static const char kBody[100] = {0};
  CHECK_LE(body_bytes, sizeof(kBody)) << "HitDecision storage is 100 bytes";
  decision.body = StringPiece(kBody, body_bytes);
  return decision;
}

TEST_F(NgxDaemonServeEmitTest, TheTwoLegsCarryTheSameVaryTokenSet) {
  // The durable statement of the contract: the arm's own Vary line is
  // IDENTICAL on the 200 and the 304 of one entry, so the legs agree by
  // construction whatever the operator configures.  It is also the only
  // `Vary` line on the wire: the server's own stamp is withdrawn whenever
  // the arm's value already lists the encoding token.
  ResponseHeaders ok_headers;
  const DaemonServeDecision ok = HitDecision("text/css", false, 100);
  const PsDaemonServeEmit ok_emit =
      PsComposeDaemonServeHeaders(ok, "text/html", 20751, true, &ok_headers);
  EXPECT_EQ("text/css", ok_emit.media_type);
  EXPECT_EQ("Accept, Accept-Encoding", ok_emit.vary);
  EXPECT_EQ(HttpStatus::kOK, ok_headers.status_code());
  EXPECT_STREQ("text/css", ok_headers.Lookup1(HttpAttributes::kContentType));
  EXPECT_STREQ("20751", ok_headers.Lookup1(HttpAttributes::kContentLength));
  EXPECT_STREQ("max-age=590, public",
               ok_headers.LookupJoined(HttpAttributes::kCacheControl).c_str());
  EXPECT_STREQ("W/\"ps-abc\"", ok_headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_STREQ("11", ok_headers.Lookup1(HttpAttributes::kAge));
  EXPECT_STREQ("Accept, Accept-Encoding",
               ok_headers.LookupJoined(HttpAttributes::kVary).c_str());
  EXPECT_EQ(nullptr, ok_headers.Lookup1(HttpAttributes::kLastModified));

  ResponseHeaders nm_headers;
  const DaemonServeDecision nm = HitDecision("text/css", true, 100);
  const PsDaemonServeEmit nm_emit =
      PsComposeDaemonServeHeaders(nm, "text/html", 20751, true, &nm_headers);
  EXPECT_EQ("Accept, Accept-Encoding", nm_emit.vary);
  EXPECT_EQ(HttpStatus::kNotModified, nm_headers.status_code());
  EXPECT_EQ(nullptr, nm_headers.Lookup1(HttpAttributes::kContentType));
  EXPECT_EQ(nullptr, nm_headers.Lookup1(HttpAttributes::kContentLength));
  EXPECT_EQ(nullptr, nm_headers.Lookup1(HttpAttributes::kLastModified));
  EXPECT_STREQ("max-age=590, public",
               nm_headers.LookupJoined(HttpAttributes::kCacheControl).c_str());
  EXPECT_STREQ("W/\"ps-abc\"", nm_headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_STREQ("11", nm_headers.Lookup1(HttpAttributes::kAge));
  // The entry's 304 carries exactly the token set its 200 carried.
  EXPECT_STREQ("Accept, Accept-Encoding",
               nm_headers.LookupJoined(HttpAttributes::kVary).c_str());
}

TEST_F(NgxDaemonServeEmitTest, ATypelessEntryStatesTheSameSetOnBothLegs) {
  // No stored Content-Type: the serving layer's resolved type feeds the
  // compressibility predicate on BOTH legs alike -- the predicate's input
  // must not depend on the leg, or a typeless entry states the token on the
  // 200 and not on the 304.
  ResponseHeaders ok_headers;
  const DaemonServeDecision ok = HitDecision("", false, 100);
  PsComposeDaemonServeHeaders(ok, "text/css", 20751, true, &ok_headers);
  ResponseHeaders nm_headers;
  const DaemonServeDecision nm = HitDecision("", true, 100);
  PsComposeDaemonServeHeaders(nm, "text/css", 20751, true, &nm_headers);
  EXPECT_STREQ("Accept, Accept-Encoding",
               ok_headers.LookupJoined(HttpAttributes::kVary).c_str());
  EXPECT_STREQ(ok_headers.LookupJoined(HttpAttributes::kVary).c_str(),
               nm_headers.LookupJoined(HttpAttributes::kVary).c_str());
  // And the 304 still carries no Content-Type.
  EXPECT_EQ(nullptr, nm_headers.Lookup1(HttpAttributes::kContentType));
}

TEST_F(NgxDaemonServeEmitTest, TheEncodingTokenCoversTypesIsCompressibleNames) {
  // The OR's second arm: the caller's bit is false (the server's own type
  // list does not name it) and IsCompressible() still states the token --
  // application/xml, application/xhtml+xml and CE-HTML live here.
  ResponseHeaders headers;
  const DaemonServeDecision decision =
      HitDecision("application/xml", false, 100);
  PsComposeDaemonServeHeaders(decision, "text/html", 4000, false, &headers);
  EXPECT_STREQ("Accept, Accept-Encoding",
               headers.LookupJoined(HttpAttributes::kVary).c_str());
}

TEST_F(NgxDaemonServeEmitTest, TheEncodingTokenHasNoSizeFloor) {
  // 20, 68 and 69 bytes: the server's gzip_min_length default, the old
  // composition's mirrored pass-through, and one past it.  There is no
  // floor anymore -- every size carries the identical set on both legs.
  for (const size_t body_bytes : {20UL, 68UL, 69UL}) {
    ResponseHeaders ok_headers;
    const DaemonServeDecision ok = HitDecision("text/css", false, body_bytes);
    PsComposeDaemonServeHeaders(ok, "text/html", body_bytes, true, &ok_headers);
    EXPECT_STREQ("Accept, Accept-Encoding",
                 ok_headers.LookupJoined(HttpAttributes::kVary).c_str())
        << body_bytes << " bytes on the 200 leg";

    ResponseHeaders nm_headers;
    const DaemonServeDecision nm = HitDecision("text/css", true, body_bytes);
    PsComposeDaemonServeHeaders(nm, "text/html", body_bytes, true, &nm_headers);
    EXPECT_STREQ("Accept, Accept-Encoding",
                 nm_headers.LookupJoined(HttpAttributes::kVary).c_str())
        << body_bytes << " bytes on the 304 leg";
  }
}

TEST_F(NgxDaemonServeEmitTest, TheEncodingTokenFollowsTheServedMediaType) {
  ResponseHeaders headers;
  const DaemonServeDecision decision = HitDecision("image/png", false, 100);
  const PsDaemonServeEmit emit =
      PsComposeDaemonServeHeaders(decision, "text/html", 4514, false, &headers);
  // An image is not a compressible type for the server's compressor: the
  // field keeps only the Accept axis.
  EXPECT_STREQ("Accept", headers.LookupJoined(HttpAttributes::kVary).c_str());
  EXPECT_EQ("Accept", emit.vary);
  EXPECT_STREQ("image/png", headers.Lookup1(HttpAttributes::kContentType));
}

TEST_F(NgxDaemonServeEmitTest, TheEncodingTokenCoversTheServerOnlyTypes) {
  // application/pdf is named by the server's own gzip type list but not by
  // ContentType::IsCompressible(): the caller's bit is what states the
  // token for it, on both legs alike.
  ResponseHeaders pdf_headers;
  const DaemonServeDecision pdf = HitDecision("application/pdf", false, 100);
  PsComposeDaemonServeHeaders(pdf, "text/html", 4000, true, &pdf_headers);
  EXPECT_STREQ("Accept, Accept-Encoding",
               pdf_headers.LookupJoined(HttpAttributes::kVary).c_str());

  ResponseHeaders pdf_nm_headers;
  const DaemonServeDecision pdf_nm = HitDecision("application/pdf", true, 100);
  PsComposeDaemonServeHeaders(pdf_nm, "text/html", 4000, true, &pdf_nm_headers);
  EXPECT_STREQ("Accept, Accept-Encoding",
               pdf_nm_headers.LookupJoined(HttpAttributes::kVary).c_str());

  // Without the caller's bit the type is not compressible at all: the
  // token is the caller's to supply, the composer does not second-guess it.
  ResponseHeaders plain_headers;
  const DaemonServeDecision plain = HitDecision("application/pdf", false, 100);
  PsComposeDaemonServeHeaders(plain, "text/html", 4000, false, &plain_headers);
  EXPECT_STREQ("Accept",
               plain_headers.LookupJoined(HttpAttributes::kVary).c_str());
}

TEST_F(NgxDaemonServeEmitTest, WithoutTheAcceptAxisTheEncodingStandsAlone) {
  ResponseHeaders ok_headers;
  DaemonServeDecision ok = HitDecision("text/css", false, 100);
  ok.emit_vary_accept = false;
  PsComposeDaemonServeHeaders(ok, "text/html", 20751, true, &ok_headers);
  EXPECT_STREQ("Accept-Encoding",
               ok_headers.LookupJoined(HttpAttributes::kVary).c_str());

  ResponseHeaders nm_headers;
  DaemonServeDecision nm = HitDecision("text/css", true, 100);
  nm.emit_vary_accept = false;
  PsComposeDaemonServeHeaders(nm, "text/html", 20751, true, &nm_headers);
  EXPECT_STREQ("Accept-Encoding",
               nm_headers.LookupJoined(HttpAttributes::kVary).c_str());
}

TEST_F(NgxDaemonServeEmitTest, TheMediaTypeFallsBackToTheServingLayersType) {
  ResponseHeaders headers;
  const DaemonServeDecision decision = HitDecision("", false, 100);
  PsComposeDaemonServeHeaders(decision, "text/css", 20751, true, &headers);
  EXPECT_STREQ("text/css", headers.Lookup1(HttpAttributes::kContentType));
  // And the composition is evaluated against that same type: text/css is
  // compressible, so both legs carry the encoding token as well.
  EXPECT_STREQ("Accept, Accept-Encoding",
               headers.LookupJoined(HttpAttributes::kVary).c_str());
}

TEST_F(NgxDaemonServeEmitTest, LastModifiedOnlyOnAnOriginalServeWithADate) {
  ResponseHeaders headers;
  DaemonServeDecision original = HitDecision("image/jpeg", false, 100);
  original.verdict = DaemonServeVerdict::kServeOriginal;
  original.origin_last_modified = 1758000000;
  PsComposeDaemonServeHeaders(original, "text/html", 98051, false, &headers);
  EXPECT_TRUE(headers.Lookup1(HttpAttributes::kLastModified) != nullptr);

  ResponseHeaders variant_headers;
  DaemonServeDecision variant = HitDecision("image/jpeg", false, 100);
  variant.verdict = DaemonServeVerdict::kServeOptimized;
  variant.origin_last_modified = 1758000000;
  PsComposeDaemonServeHeaders(variant, "text/html", 33160, false,
                              &variant_headers);
  EXPECT_EQ(nullptr, variant_headers.Lookup1(HttpAttributes::kLastModified))
      << "the origin's validator rode on bytes it never produced";
}

TEST_F(NgxDaemonServeEmitTest, EmptyDecisionPartsStayAbsent) {
  ResponseHeaders headers;
  DaemonServeDecision decision;
  decision.verdict = DaemonServeVerdict::kServeOptimized;
  decision.not_modified = false;
  decision.emit_vary_accept = false;
  const PsDaemonServeEmit emit =
      PsComposeDaemonServeHeaders(decision, "image/png", 462, false, &headers);
  EXPECT_EQ("image/png", emit.media_type);
  EXPECT_EQ("", emit.vary);
  EXPECT_STREQ("", headers.LookupJoined(HttpAttributes::kCacheControl).c_str());
  EXPECT_EQ(nullptr, headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_STREQ("", headers.LookupJoined(HttpAttributes::kVary).c_str());
  EXPECT_STREQ("0", headers.Lookup1(HttpAttributes::kAge));
}

TEST_F(NgxDaemonServeEmitTest, AStoredCodedCopyIsLabelledOnTheTwoHundredOnly) {
  // The coding is the decision's -- read off the stored entry by the arm --
  // and it labels the body, so it rides on the 200 and never on the 304.
  // Copied into headers_out.content_encoding by the header copy, it is what
  // nginx's gzip and brotli filters check before compressing.
  DaemonServeDecision ok = HitDecision("text/css", false, 100);
  ok.content_encoding = kServeContentEncodingBrotli;
  ResponseHeaders ok_headers;
  const PsDaemonServeEmit ok_emit =
      PsComposeDaemonServeHeaders(ok, "", 100, false, &ok_headers);
  EXPECT_STREQ("br", ok_headers.Lookup1(HttpAttributes::kContentEncoding));
  // The length is the stored copy's, as passed.
  EXPECT_STREQ("100", ok_headers.Lookup1(HttpAttributes::kContentLength));
  EXPECT_EQ("Accept, Accept-Encoding", ok_emit.vary);

  DaemonServeDecision nm = HitDecision("text/css", true, 100);
  nm.content_encoding = kServeContentEncodingBrotli;
  ResponseHeaders nm_headers;
  const PsDaemonServeEmit nm_emit =
      PsComposeDaemonServeHeaders(nm, "", 100, false, &nm_headers);
  EXPECT_EQ(nullptr, nm_headers.Lookup1(HttpAttributes::kContentEncoding));
  EXPECT_EQ(ok_emit.vary, nm_emit.vary);
  EXPECT_STREQ("Accept, Accept-Encoding",
               nm_headers.LookupJoined(HttpAttributes::kVary).c_str());

  DaemonServeDecision gzip = HitDecision("application/javascript", false, 64);
  gzip.content_encoding = kServeContentEncodingGzip;
  ResponseHeaders gzip_headers;
  PsComposeDaemonServeHeaders(gzip, "", 64, true, &gzip_headers);
  EXPECT_STREQ("gzip", gzip_headers.Lookup1(HttpAttributes::kContentEncoding));
}

TEST_F(NgxDaemonServeEmitTest, AnUncodedCopyCarriesNoContentEncoding) {
  ResponseHeaders headers;
  PsComposeDaemonServeHeaders(HitDecision("text/css", false, 100), "", 100,
                              true, &headers);
  EXPECT_EQ(nullptr, headers.Lookup1(HttpAttributes::kContentEncoding));
}

TEST_F(NgxDaemonServeEmitTest, ACodedImageStillVariesOnTheEncoding) {
  // A media type neither the server's list nor IsCompressible() names: an
  // uncoded copy of it states only the Accept axis, a coded one adds the
  // encoding axis on both legs -- its bytes vary on Accept-Encoding by
  // construction, compressor or not.
  ResponseHeaders plain_headers;
  const PsDaemonServeEmit plain = PsComposeDaemonServeHeaders(
      HitDecision("image/png", false, 100), "", 100, false, &plain_headers);
  EXPECT_EQ("Accept", plain.vary);

  DaemonServeDecision coded = HitDecision("image/png", false, 100);
  coded.content_encoding = kServeContentEncodingGzip;
  ResponseHeaders coded_headers;
  const PsDaemonServeEmit coded_emit =
      PsComposeDaemonServeHeaders(coded, "", 100, false, &coded_headers);
  EXPECT_EQ("Accept, Accept-Encoding", coded_emit.vary);
}

}  // namespace
}  // namespace net_instaweb

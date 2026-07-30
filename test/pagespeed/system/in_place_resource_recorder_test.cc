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

// Unit tests for InPlaceResourceRecorder.

#include <memory>
#include <vector>

#include "pagespeed/system/in_place_resource_recorder.h"

#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_cache_failure.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {

namespace {

const int kMaxResponseBytes = 1024;
const char kTestUrl[] = "http://www.example.com/";
const char kHello[] = "Hello, IPRO.";
const char kBye[] = "Bye IPRO.";

const char kUncompressedData[] = "Hello";

// This was generated with 'xxd -i hello.gz' after gzipping a file with "Hello".
const unsigned char kGzippedData[] = {
    0x1f, 0x8b, 0x08, 0x08, 0x3b, 0x3a, 0xf3, 0x4e, 0x00, 0x03, 0x68,
    0x65, 0x6c, 0x6c, 0x6f, 0x00, 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x07,
    0x00, 0x82, 0x89, 0xd1, 0xf7, 0x05, 0x00, 0x00, 0x00};

class InPlaceResourceRecorderTest : public RewriteTestBase {
 protected:
  enum GzipHeaderTime { kPrelimGzipHeader, kLateGzipHeader };

  void SetUp() override {
    RewriteTestBase::SetUp();
    InPlaceResourceRecorder::InitStats(statistics());
  }

  InPlaceResourceRecorder* MakeRecorder(StringPiece url) {
    RequestHeaders headers;
    return new InPlaceResourceRecorder(
        RequestContext::NewTestRequestContext(
            server_context()->thread_system()),
        url, rewrite_driver_->CacheFragment(), headers.GetProperties(),
        kMaxResponseBytes, 4, /* max_concurrent_recordings*/
        http_cache(), statistics(), message_handler());
  }

  void TestWithGzip(GzipHeaderTime header_time) {
    ResponseHeaders prelim_headers;
    prelim_headers.set_status_code(HttpStatus::kOK);
    if (header_time == kPrelimGzipHeader) {
      prelim_headers.Add(HttpAttributes::kContentEncoding,
                         HttpAttributes::kGzip);
    }

    ResponseHeaders final_headers;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &final_headers);
    if ((header_time == kLateGzipHeader) ||
        (header_time == kPrelimGzipHeader)) {
      final_headers.Add(HttpAttributes::kContentEncoding,
                        HttpAttributes::kGzip);
    }
    final_headers.ComputeCaching();

    std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
    recorder->ConsiderResponseHeaders(
        InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);

    StringPiece gzipped(reinterpret_cast<const char*>(&kGzippedData[0]),
                        STATIC_STRLEN(kGzippedData));
    recorder->Write(gzipped, message_handler());
    recorder.release()->DoneAndSetHeaders(&final_headers,
                                          true /* complete response */);

    HTTPValue value_out;
    ResponseHeaders headers_out;
    EXPECT_EQ(kFoundResult, HttpBlockingFind(kTestUrl, http_cache(), &value_out,
                                             &headers_out));
    StringPiece contents;
    EXPECT_TRUE(value_out.ExtractContents(&contents));
    EXPECT_EQ(headers_out.IsGzipped() ? gzipped : kUncompressedData, contents);

    // We should not have a content-encoding header since we either decompressed
    // data ourselves or captured it before data was saved.
    EXPECT_FALSE(headers_out.Has(HttpAttributes::kContentEncoding));
    EXPECT_TRUE(headers_out.DetermineContentType()->IsCompressible());

    // TODO(jcrowell): Add test for non-compressible type.
  }

  void CheckCacheableContentType(const ContentType* content_type) {
    ResponseHeaders headers;
    SetDefaultLongCacheHeaders(content_type, &headers);
    std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
    recorder->ConsiderResponseHeaders(InPlaceResourceRecorder::kFullHeaders,
                                      &headers);
    EXPECT_FALSE(recorder->failed());
    HTTPValue value_out;
    ResponseHeaders headers_out;
    EXPECT_EQ(
        kNotFoundResult,  // Check it wasn't cached as 'not cacheable'.
        HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));
  }

  // Returns the HTTP-Cache result associated with requesting a content-type
  // that is not cacheable.
  HTTPCache::FindResult NotCacheableContentType(
      const ContentType* content_type,
      InPlaceResourceRecorder::HeadersKind headers_kind, bool expect_failure) {
    ResponseHeaders headers;
    SetDefaultLongCacheHeaders(content_type, &headers);
    std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
    recorder->ConsiderResponseHeaders(headers_kind, &headers);
    EXPECT_EQ(expect_failure, recorder->failed());
    HTTPValue value_out;
    ResponseHeaders headers_out;
    return HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out);
  }

  const char* BailsForContentType(const char* mime_type) {
    ResponseHeaders headers;
    headers.set_status_code(HttpStatus::kOK);
    SetDefaultLongCacheHeaders(&kContentTypeCss, &headers);
    if (mime_type == nullptr) {
      headers.RemoveAll(HttpAttributes::kContentType);
    } else {
      headers.Replace(HttpAttributes::kContentType, mime_type);
    }
    headers.ComputeCaching();

    std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
    recorder->ConsiderResponseHeaders(
        InPlaceResourceRecorder::kPreliminaryHeaders, &headers);
    if (recorder->failed()) {
      return "prelim";
    }
    recorder->ConsiderResponseHeaders(InPlaceResourceRecorder::kFullHeaders,
                                      &headers);
    if (recorder->failed()) {
      return "full";
    }
    return "none";
  }

  int64 IproVar(const char* name) {
    return statistics()->GetVariable(name)->Get();
  }

  // Every recording ends in exactly one terminal outcome: it is either
  // inserted into cache or lands in one specific counter. The specific
  // counters are mutually exclusive, so their sum plus the inserts must equal
  // the number of recordings started. Valid for scenarios where every
  // recorder that was constructed is driven to a terminal state.
  void ExpectTerminalInvariant() {
    int64 terminal = IproVar("ipro_recorder_inserted_into_cache") +
                     IproVar("ipro_recorder_not_cacheable") +
                     IproVar("ipro_recorder_failed") +
                     IproVar("ipro_recorder_dropped_due_to_load") +
                     IproVar("ipro_recorder_dropped_due_to_size") +
                     IproVar("ipro_recorder_dropped_content_type") +
                     IproVar("ipro_recorder_error_status") +
                     IproVar("ipro_recorder_skipped_transient") +
                     IproVar("ipro_recorder_empty");
    EXPECT_EQ(IproVar("ipro_recorder_resources"), terminal);
  }

  GoogleString MessageDump() {
    GoogleString messages;
    StringWriter writer(&messages);
    message_handler()->Dump(&writer);
    return messages;
  }
};

TEST_F(InPlaceResourceRecorderTest, BasicOperation) {
  ResponseHeaders prelim_headers;
  prelim_headers.set_status_code(HttpStatus::kOK);

  ResponseHeaders ok_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &ok_headers);

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(
      InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);
  recorder->Write(kHello, message_handler());
  recorder->Write(kBye, message_handler());
  recorder.release()->DoneAndSetHeaders(&ok_headers,
                                        true /* complete response */);

  HTTPValue value_out;
  ResponseHeaders headers_out;
  EXPECT_EQ(kFoundResult,
            HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));
  StringPiece contents;
  EXPECT_TRUE(value_out.ExtractContents(&contents));
  EXPECT_EQ(StrCat(kHello, kBye), contents);

  // A successful recording inserts into cache and touches no failure/skip
  // counter.
  EXPECT_EQ(1, IproVar("ipro_recorder_inserted_into_cache"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  ExpectTerminalInvariant();
}

TEST_F(InPlaceResourceRecorderTest, IncompleteResponse) {
  ResponseHeaders prelim_headers;
  prelim_headers.set_status_code(HttpStatus::kOK);

  ResponseHeaders ok_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &ok_headers);

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(
      InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);
  recorder->Write(kHello, message_handler());
  recorder.release()->DoneAndSetHeaders(&ok_headers,
                                        false /* incomplete response */);

  HTTPValue value_out;
  ResponseHeaders headers_out;
  EXPECT_EQ(kNotFoundResult,
            HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));

  // A truncated response is a genuine recording failure: it bumps failed and
  // no more-specific counter, and logs a warning naming the URL.
  EXPECT_EQ(1, IproVar("ipro_recorder_failed"));
  EXPECT_EQ(0, IproVar("ipro_recorder_empty"));
  EXPECT_EQ(0, IproVar("ipro_recorder_inserted_into_cache"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(),
              ::testing::HasSubstr("incomplete/truncated response"));
}

TEST_F(InPlaceResourceRecorderTest, CheckCacheableContentTypes) {
  CheckCacheableContentType(&kContentTypeJpeg);
  CheckCacheableContentType(&kContentTypeCss);
  CheckCacheableContentType(&kContentTypeJavascript);
  CheckCacheableContentType(&kContentTypeJson);
}

TEST_F(InPlaceResourceRecorderTest, NotCacheableContentTypeFull) {
  EXPECT_EQ(HTTPCache::FindResult(HTTPCache::kRecentFailure,
                                  kFetchStatusUncacheable200),
            NotCacheableContentType(&kContentTypePdf,
                                    InPlaceResourceRecorder::kFullHeaders,
                                    true /* expect_failure */));
  // A non-rewritable content type is an expected bail, not a failure.
  EXPECT_EQ(1, IproVar("ipro_recorder_dropped_content_type"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("content type is not"));
}

TEST_F(InPlaceResourceRecorderTest, NotCacheableContentTypePreliminary) {
  EXPECT_EQ(HTTPCache::FindResult(HTTPCache::kNotFound, kFetchStatusNotSet),
            NotCacheableContentType(
                &kContentTypePdf, InPlaceResourceRecorder::kPreliminaryHeaders,
                true /* expect_failure */));
  // The preliminary-headers content-type bail also counts as a dropped
  // content type, never as a failure.
  EXPECT_EQ(1, IproVar("ipro_recorder_dropped_content_type"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
}

TEST_F(InPlaceResourceRecorderTest, UnknownContentTypeFull) {
  EXPECT_EQ(
      HTTPCache::FindResult(HTTPCache::kRecentFailure,
                            kFetchStatusUncacheable200),
      NotCacheableContentType(nullptr, InPlaceResourceRecorder::kFullHeaders,
                              true /* expect_failure */));
  EXPECT_EQ(1, IproVar("ipro_recorder_dropped_content_type"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  ExpectTerminalInvariant();
}

TEST_F(InPlaceResourceRecorderTest, UnknownContentTypePreliminary) {
  EXPECT_EQ(HTTPCache::FindResult(HTTPCache::kNotFound, kFetchStatusNotSet),
            NotCacheableContentType(
                nullptr, InPlaceResourceRecorder::kPreliminaryHeaders,
                false /* expect_failure */));
}

TEST_F(InPlaceResourceRecorderTest, BasicOperationFullHeaders) {
  // Deliver full headers initially. This is how nginx works.
  ResponseHeaders ok_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &ok_headers);

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(InPlaceResourceRecorder::kFullHeaders,
                                    &ok_headers);
  recorder->Write(kHello, message_handler());
  recorder->Write(kBye, message_handler());
  recorder.release()->DoneAndSetHeaders(&ok_headers,
                                        true /* complete response */);

  HTTPValue value_out;
  ResponseHeaders headers_out;
  EXPECT_EQ(kFoundResult,
            HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));
  StringPiece contents;
  EXPECT_TRUE(value_out.ExtractContents(&contents));
  EXPECT_EQ(StrCat(kHello, kBye), contents);
}

TEST_F(InPlaceResourceRecorderTest, DontRemember304) {
  ResponseHeaders prelim_headers;
  prelim_headers.set_status_code(HttpStatus::kOK);

  // 304
  ResponseHeaders not_modified_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &not_modified_headers);
  not_modified_headers.SetStatusAndReason(HttpStatus::kNotModified);
  not_modified_headers.ComputeCaching();

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(
      InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);
  recorder.release()->DoneAndSetHeaders(&not_modified_headers,
                                        true /* complete response */);

  HTTPValue value_out;
  ResponseHeaders headers_out;
  // This should be not found, not one of the RememberNot... statuses
  EXPECT_EQ(kNotFoundResult,
            HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));

  // A 304 is a transient non-200: counted as skipped, not failed.
  EXPECT_EQ(1, IproVar("ipro_recorder_skipped_transient"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  EXPECT_EQ(0, IproVar("ipro_recorder_error_status"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("transient status"));
}

TEST_F(InPlaceResourceRecorderTest, Remember500AsFetchFailed) {
  ResponseHeaders prelim_headers;
  prelim_headers.set_status_code(HttpStatus::kOK);

  ResponseHeaders error_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &error_headers);
  error_headers.SetStatusAndReason(HttpStatus::kInternalServerError);
  error_headers.ComputeCaching();

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(
      InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);
  recorder.release()->DoneAndSetHeaders(&error_headers,
                                        true /* complete response */);

  HTTPValue value_out;
  ResponseHeaders headers_out;
  // For 500 we do remember fetch failed.
  EXPECT_EQ(
      HTTPCache::FindResult(HTTPCache::kRecentFailure, kFetchStatusOtherError),
      HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));

  // A 5xx origin response is a broken-resource signal, counted separately
  // from genuine recorder failures.
  EXPECT_EQ(1, IproVar("ipro_recorder_error_status"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("origin returned 500"));
}

TEST_F(InPlaceResourceRecorderTest, RememberEmpty) {
  ResponseHeaders ok_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &ok_headers);

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  // No contents written.
  recorder.release()->DoneAndSetHeaders(&ok_headers,
                                        true /* complete response */);

  HTTPValue value_out;
  ResponseHeaders headers_out;
  // Remember recent empty.
  EXPECT_EQ(HTTPCache::FindResult(HTTPCache::kRecentFailure, kFetchStatusEmpty),
            HttpBlockingFind(kTestUrl, http_cache(), &value_out, &headers_out));

  // A legal empty 200 is a deliberate cache-the-skip, not a malfunction.
  EXPECT_EQ(1, IproVar("ipro_recorder_empty"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("empty 200 response"));
}

TEST_F(InPlaceResourceRecorderTest, DecompressGzipIfNeeded) {
  // Test where we get already-gzip'd content, as shown by preliminary headers.
  // This corresponds to reverse proxy cases.
  DisableGzip();
  TestWithGzip(kPrelimGzipHeader);
}

TEST_F(InPlaceResourceRecorderTest, SplitOperationWithGzip) {
  // Test that gzip on non-prelim headers don't cause gunzip'ing.
  // This is to permit capture of headers after mod_deflate has run.
  DisableGzip();
  TestWithGzip(kLateGzipHeader);
}

TEST_F(InPlaceResourceRecorderTest, DecompressGzipIfNeededWithCompressedCache) {
  // Test where we get already-gzip'd content, as shown by preliminary headers.
  // This corresponds to reverse proxy cases.
  TestWithGzip(kPrelimGzipHeader);
}

TEST_F(InPlaceResourceRecorderTest, SplitOperationWithGzipWithCompressedCache) {
  // Test that gzip on non-prelim headers don't cause gunzip'ing.
  // This is to permit capture of headers after mod_deflate has run.
  TestWithGzip(kLateGzipHeader);
}

TEST_F(InPlaceResourceRecorderTest, BailEarlyOnUnexpectedContentType) {
  EXPECT_STREQ("prelim", BailsForContentType(kContentTypeHtml.mime_type()));
  EXPECT_STREQ("prelim", BailsForContentType(kContentTypePdf.mime_type()));
  EXPECT_STREQ("prelim", BailsForContentType(kContentTypeText.mime_type()));
  EXPECT_STREQ("prelim", BailsForContentType("bogus"));
  EXPECT_STREQ("none", BailsForContentType(kContentTypeCss.mime_type()));
  EXPECT_STREQ("none", BailsForContentType(kContentTypeJavascript.mime_type()));
  EXPECT_STREQ("none", BailsForContentType(kContentTypeGif.mime_type()));
  EXPECT_STREQ("none", BailsForContentType(kContentTypePng.mime_type()));
  EXPECT_STREQ("none", BailsForContentType(kContentTypeJpeg.mime_type()));
  EXPECT_STREQ("none", BailsForContentType(kContentTypeWebp.mime_type()));

  // Note that if the content-type is missing in the first round, we
  // don't bail early, but we will bail late.  This is because in
  // the preliminary round, we may not know the correct content type
  // yet, so we need to be conservative and let the processing continue.
  EXPECT_STREQ("full", BailsForContentType(nullptr));
}

TEST_F(InPlaceResourceRecorderTest,
       NotCacheableCacheControlCountsNotCacheable) {
  // A rewritable content type (CSS) whose Cache-Control forbids proxy caching
  // reaches the is-cacheable check and bails there: an expected policy
  // outcome, counted as not-cacheable and never as a failure.
  ResponseHeaders headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &headers);
  headers.Replace(HttpAttributes::kCacheControl, "private, max-age=300");
  headers.ComputeCaching();

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(InPlaceResourceRecorder::kFullHeaders,
                                    &headers);
  EXPECT_TRUE(recorder->failed());
  recorder.release()->DoneAndSetHeaders(&headers, true /* complete response */);

  EXPECT_EQ(1, IproVar("ipro_recorder_not_cacheable"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  EXPECT_EQ(0, IproVar("ipro_recorder_dropped_content_type"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("not cacheable"));
}

TEST_F(InPlaceResourceRecorderTest, OversizeCountsDroppedDueToSize) {
  // Writing more than max_response_bytes bumps the size counter, not failed.
  ResponseHeaders prelim_headers;
  prelim_headers.set_status_code(HttpStatus::kOK);
  ResponseHeaders ok_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &ok_headers);

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(
      InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);
  const GoogleString too_big(kMaxResponseBytes + 100, 'a');
  recorder->Write(too_big, message_handler());
  recorder.release()->DoneAndSetHeaders(&ok_headers, true /* complete */);

  EXPECT_EQ(1, IproVar("ipro_recorder_dropped_due_to_size"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("exceeds the size limit"));
}

TEST_F(InPlaceResourceRecorderTest, LoadShedCountsDroppedDueToLoad) {
  // MakeRecorder allows 4 concurrent recordings. Hold 4 alive to fill the
  // slots so the 5th is load-shed at construction: it counts as dropped due
  // to load, never as a failure. (No terminal invariant here -- the 4
  // blockers are deliberately abandoned mid-recording.)
  std::vector<std::unique_ptr<InPlaceResourceRecorder>> blockers;
  for (int i = 0; i < 4; ++i) {
    blockers.emplace_back(MakeRecorder(kTestUrl));
  }
  InPlaceResourceRecorder* shed = MakeRecorder(kTestUrl);
  EXPECT_TRUE(shed->failed());

  ResponseHeaders ok_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &ok_headers);
  shed->DoneAndSetHeaders(&ok_headers, true /* complete response */);

  EXPECT_EQ(1, IproVar("ipro_recorder_dropped_due_to_load"));
  EXPECT_EQ(0, IproVar("ipro_recorder_failed"));
  EXPECT_THAT(MessageDump(),
              ::testing::HasSubstr("too many concurrent recordings"));
}

TEST_F(InPlaceResourceRecorderTest, WriteInflateFailureCountsFailed) {
  // A response declared gzip whose body is not valid gzip makes the inflater
  // error: a genuine recording malfunction, counted as failed and logged at
  // warning severity with the URL.
  DisableGzip();
  ResponseHeaders prelim_headers;
  prelim_headers.set_status_code(HttpStatus::kOK);
  prelim_headers.Add(HttpAttributes::kContentEncoding, HttpAttributes::kGzip);

  ResponseHeaders final_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &final_headers);
  final_headers.Add(HttpAttributes::kContentEncoding, HttpAttributes::kGzip);
  final_headers.ComputeCaching();

  std::unique_ptr<InPlaceResourceRecorder> recorder(MakeRecorder(kTestUrl));
  recorder->ConsiderResponseHeaders(
      InPlaceResourceRecorder::kPreliminaryHeaders, &prelim_headers);
  recorder->Write("this is definitely not valid gzip data, no magic bytes",
                  message_handler());
  recorder.release()->DoneAndSetHeaders(&final_headers, true /* complete */);

  EXPECT_EQ(1, IproVar("ipro_recorder_failed"));
  EXPECT_EQ(0, IproVar("ipro_recorder_dropped_due_to_size"));
  EXPECT_EQ(0, IproVar("ipro_recorder_empty"));
  ExpectTerminalInvariant();
  EXPECT_THAT(MessageDump(), ::testing::HasSubstr("write/inflate error"));
}

}  // namespace

}  // namespace net_instaweb

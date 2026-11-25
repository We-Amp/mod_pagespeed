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

// The body walk of the in-place recording filter, against real APR buckets:
// a response the recorder will not record is not read, so its FILE buckets
// are still FILE buckets when the brigade moves on; a recorded one is read
// exactly as before.

#include "pagespeed/apache/in_place_record_brigade.h"

#include <cstring>

#include "apr_buckets.h"  // NOLINT
#include "apr_file_io.h"  // NOLINT
#include "apr_pools.h"    // NOLINT
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/ipro_recorder.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"
// clang-format off: http_protocol.h requires apache_httpd_includes.h
// (httpd.h) first; alphabetical include sorting breaks them.
#include "pagespeed/apache/apache_httpd_includes.h"
#include "http_protocol.h"  // NOLINT - for ap_bucket_type_error
// clang-format on

namespace net_instaweb {
namespace {

const char kFileBody[] = "body { color: red; } /* a static file */";

// A recorder that stops wanting the body when its headers say HTML, or
// once it holds `stop_after` bytes (the shape of a body past a cap).
// Never finished: DoneAndSetHeaders would delete a stack object.
class ScriptedRecorder : public IproRecorder {
 public:
  ScriptedRecorder(bool html, size_t stop_after = 0)
      : html_(html), stop_after_(stop_after) {}

  bool Write(const StringPiece& contents, MessageHandler* handler) override {
    contents.AppendToString(&written_);
    if (stop_after_ > 0 && written_.size() >= stop_after_) {
      declined_ = true;
    }
    return true;
  }
  bool Flush(MessageHandler* handler) override { return true; }
  void ConsiderResponseHeaders(HeadersKind headers_kind,
                               ResponseHeaders* response_headers) override {
    ++offers_;
    if (html_) {
      declined_ = true;
    }
  }
  void SaveCacheControl(const char* cache_control) override {}
  void Fail() override { failed_ = true; }
  bool failed() const override { return failed_; }
  bool wants_body() const override { return !failed_ && !declined_; }
  MessageHandler* handler() override { return nullptr; }
  const HttpOptions& http_options() const override { return http_options_; }
  void DoneAndSetHeaders(ResponseHeaders* response_headers,
                         bool entire_response_received) override {}
  const GoogleString& url() const override { return url_; }

  const GoogleString& written() const { return written_; }
  int offers() const { return offers_; }

 private:
  const bool html_;
  const size_t stop_after_;
  bool declined_ = false;
  bool failed_ = false;
  int offers_ = 0;
  GoogleString written_;
  HttpOptions http_options_;
  GoogleString url_;
};

class InPlaceRecordBrigadeTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();
    apr_pool_create(&pool_, nullptr);
    alloc_ = apr_bucket_alloc_create(pool_);
    bb_ = apr_brigade_create(pool_, alloc_);
  }

  void TearDown() override {
    apr_brigade_destroy(bb_);
    apr_bucket_alloc_destroy(alloc_);
    apr_pool_destroy(pool_);
    MockApache::Terminate();
  }

  // Appends a FILE bucket over a temporary file holding kFileBody.
  void AppendFileBucket() {
    GoogleString path = StrCat(GTestTempDir(), "/in_place_record_XXXXXX");
    apr_file_t* file = nullptr;
    ASSERT_EQ(
        APR_SUCCESS,
        apr_file_mktemp(&file, &path[0],
                        APR_FOPEN_CREATE | APR_FOPEN_READ | APR_FOPEN_WRITE |
                            APR_FOPEN_EXCL | APR_FOPEN_DELONCLOSE,
                        pool_));
    apr_size_t len = strlen(kFileBody);
    ASSERT_EQ(APR_SUCCESS, apr_file_write(file, kFileBody, &len));
    APR_BRIGADE_INSERT_TAIL(
        bb_, apr_bucket_file_create(file, 0, strlen(kFileBody), pool_, alloc_));
  }

  void AppendHeap(const char* text) {
    APR_BRIGADE_INSERT_TAIL(
        bb_, apr_bucket_heap_create(text, strlen(text), nullptr, alloc_));
  }

  void AppendError() {
    apr_bucket* bucket =
        static_cast<apr_bucket*>(apr_bucket_alloc(sizeof(*bucket), alloc_));
    APR_BUCKET_INIT(bucket);
    bucket->free = apr_bucket_free;
    bucket->list = alloc_;
    bucket->type = &ap_bucket_type_error;
    bucket->length = 0;
    bucket->start = 0;
    bucket->data = nullptr;
    APR_BRIGADE_INSERT_TAIL(bb_, bucket);
  }

  void AppendEos() {
    APR_BRIGADE_INSERT_TAIL(bb_, apr_bucket_eos_create(alloc_));
  }

  apr_status_t Feed(ScriptedRecorder* recorder) {
    return FeedBrigadeToInPlaceRecorder(bb_, recorder, [recorder]() {
      ResponseHeaders headers;
      recorder->ConsiderResponseHeaders(IproRecorder::kPreliminaryHeaders,
                                        &headers);
    });
  }

  // The number of FILE buckets still in the brigade.
  int FileBuckets() const {
    int count = 0;
    for (apr_bucket* bucket = APR_BRIGADE_FIRST(bb_);
         bucket != APR_BRIGADE_SENTINEL(bb_);
         bucket = APR_BUCKET_NEXT(bucket)) {
      if (APR_BUCKET_IS_FILE(bucket)) {
        ++count;
      }
    }
    return count;
  }

  apr_pool_t* pool_ = nullptr;
  apr_bucket_alloc_t* alloc_ = nullptr;
  apr_bucket_brigade* bb_ = nullptr;
};

TEST_F(InPlaceRecordBrigadeTest, HtmlIsDecidedAtTheHeadersAndNeverRead) {
  AppendFileBucket();
  AppendEos();
  ScriptedRecorder recorder(true /* html */);
  EXPECT_EQ(APR_SUCCESS, Feed(&recorder));
  EXPECT_EQ(1, recorder.offers());
  EXPECT_EQ("", recorder.written());
  EXPECT_EQ(1, FileBuckets()) << "the FILE bucket was read into memory";
  apr_off_t length = 0;
  ASSERT_EQ(APR_SUCCESS, apr_brigade_length(bb_, 0 /* read_all */, &length));
  EXPECT_EQ(static_cast<apr_off_t>(strlen(kFileBody)), length);
}

TEST_F(InPlaceRecordBrigadeTest, AResourceIsReadAsBefore) {
  AppendFileBucket();
  AppendEos();
  ScriptedRecorder recorder(false /* html */);
  EXPECT_EQ(APR_SUCCESS, Feed(&recorder));
  EXPECT_EQ(1, recorder.offers());
  EXPECT_EQ(kFileBody, recorder.written());
  EXPECT_EQ(0, FileBuckets());
}

TEST_F(InPlaceRecordBrigadeTest, TheWalkStopsOnceTheBodyIsNoLongerWanted) {
  AppendHeap("abcd");
  AppendFileBucket();
  AppendEos();
  ScriptedRecorder recorder(false /* html */, 4 /* stop_after */);
  EXPECT_EQ(APR_SUCCESS, Feed(&recorder));
  EXPECT_EQ("abcd", recorder.written());
  EXPECT_EQ(1, FileBuckets()) << "a bucket past the decision was read";
}

TEST_F(InPlaceRecordBrigadeTest, ALaterBrigadeOfADeclinedBodyIsNotTouched) {
  // Apache hands the body over in several brigades.  After the first one
  // decided the response, the next is neither offered nor read.
  ScriptedRecorder recorder(true /* html */);
  AppendHeap("<html>");
  EXPECT_EQ(APR_SUCCESS, Feed(&recorder));
  apr_brigade_cleanup(bb_);
  AppendFileBucket();
  AppendEos();
  EXPECT_EQ(APR_SUCCESS, Feed(&recorder));
  EXPECT_EQ(1, recorder.offers());
  EXPECT_EQ("", recorder.written());
  EXPECT_EQ(1, FileBuckets());
}

TEST_F(InPlaceRecordBrigadeTest, AnErrorBucketFailsTheRecording) {
  AppendHeap("abcd");
  AppendError();
  AppendFileBucket();
  AppendEos();
  ScriptedRecorder recorder(false /* html */);
  EXPECT_EQ(APR_SUCCESS, Feed(&recorder));
  EXPECT_TRUE(recorder.failed());
  EXPECT_EQ("abcd", recorder.written());
  EXPECT_EQ(1, FileBuckets());
}

}  // namespace
}  // namespace net_instaweb

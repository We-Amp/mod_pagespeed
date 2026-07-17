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

#include "pagespeed/apache/apache_fetch.h"

#include <cstring>
#include <memory>
#include <vector>

#include "apr_buckets.h"  // NOLINT - bucket allocator for the aliased serve
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/rewriter/public/resource_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/apache/apache_httpd_includes.h"
#include "pagespeed/apache/header_util.h"
#include "pagespeed/apache/streaming_pagespeed_resource_fetch.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/cache/delay_cache.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "util_filter.h"  // NOLINT - ap_filter_t/ap_filter_rec_t for the
                          // zero-copy verbatim-chain test setup

namespace {

const char kJsData[] = "alert ( 'hello, world!' ) ";
const char kJsUrl[] = "http://example.com/foo.js";
const char kExampleUrl[] = "http://www.example.com";
const char kCacheFragment[] = "";
const char kDebugInfo[] = "ignored";
const char kExpectedHeaders[] =
    "Content-Type: text/plain\n"
    "Set-Cookie: test=cookie\n"
    "Set-Cookie: tasty=cookie\n"
    "Set-Cookie2: obselete\n"
    "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
    "X-Content-Type-Options: nosniff\n"
    "Cache-Control: max-age=0, no-cache\n";

}  // namespace

namespace net_instaweb {

class ApacheFetchTest : public RewriteTestBase {
 public:
  ApacheFetchTest()
      : apache_writer_(
            new ApacheWriter(&request_, server_context_->thread_system())),
        request_headers_(new RequestHeaders()),
        request_ctx_(RequestContext::NewTestRequestContextWithTimer(
            server_context_->thread_system(), timer())) {}

  void SetUp() override {
    RewriteTestBase::SetUp();

    MockApache::Initialize();
    MockApache::PrepareRequest(&request_);
  }

  void TearDown() override {
    apache_fetch_.reset(nullptr);
    RewriteTestBase::TearDown();
  }

  void InitFetchUnbuffered(StringPiece url, HttpStatus::Code code) {
    InitFetch(url, code, false);
  }

  void InitFetchBuffered(StringPiece url, HttpStatus::Code code) {
    InitFetch(url, code, true);
  }

  void InitFetch(StringPiece url, HttpStatus::Code code, bool buffered) {
    // Takes ownership of apache_writer and request_headers but nothing else.
    // Keeping a copy them violates ApacheFetch's mutex guarantees, and should
    // only be done by tests.
    apache_fetch_ = std::make_unique<ApacheFetch>(
        url.as_string(), kDebugInfo, rewrite_driver_, apache_writer_,
        request_headers_, request_ctx_, options(), message_handler());
    apache_fetch_->set_buffered(buffered);
    apache_fetch_->response_headers()->SetStatusAndReason(code);

    // HTTP 1.1 is most common now.
    apache_fetch_->response_headers()->set_major_version(1);
    apache_fetch_->response_headers()->set_minor_version(1);

    // Responses need a content type or we will 403.
    apache_fetch_->response_headers()->MergeContentType("text/plain");

    // Set cookies to make sure they get removed.
    apache_fetch_->response_headers()->Add("Set-Cookie", "test=cookie");
    apache_fetch_->response_headers()->Add("Set-Cookie", "tasty=cookie");
    apache_fetch_->response_headers()->Add("Set-Cookie2", "obselete");

    // None of these should have been passed through to the apache_writer.
    EXPECT_STREQ("", MockApache::ActionsSinceLastCall());
  }

  ~ApacheFetchTest() override {
    MockApache::CleanupRequest(&request_);
    MockApache::Terminate();
  }

  void WaitIproTest(bool buffered) {
    // Run through a successful completion and verify that no issues were
    // sent to the message handler.
    AddFilter(RewriteOptions::kRewriteJavascriptExternal);
    ResponseHeaders response_headers;
    DefaultResponseHeaders(kContentTypeJavascript, 600, &response_headers);
    RequestHeaders::Properties req_properties;
    req_properties.has_cookie = false;
    req_properties.has_cookie2 = false;
    http_cache()->Put(kJsUrl, kCacheFragment, req_properties,
                      ResponseHeaders::kIgnoreVaryOnResources,
                      &response_headers, kJsData, &message_handler_);

    GoogleUrl gurl(kJsUrl);
    InitFetch(kJsUrl, HttpStatus::kOK, buffered);
    rewrite_driver_->FetchInPlaceResource(gurl, false /* proxy_mode */,
                                          apache_fetch_.get());
    apache_fetch_->Wait();
    EXPECT_EQ(0, message_handler()->TotalMessages());

    EXPECT_EQ(StrCat("ap_set_content_length(26) "
                     "ap_set_content_type(application/javascript) "
                     "ap_remove_output_filter(MOD_EXPIRES) "
                     "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
                     "ap_set_content_type(application/javascript) "
                     "ap_rwrite(",
                     kJsData, ")"),
              MockApache::ActionsSinceLastCall());
    apache_fetch_.reset(nullptr);
  }

  void ReleaseKey(GoogleString key) { delay_cache()->ReleaseKey(key); }

 protected:
  void FetchDone() { apache_fetch_->Done(true); }

  // After a request is all set up, run through a successful completion and
  // verify that no issues were sent to the message handler.
  void WaitExpectSuccess() {
    rewrite_driver_->scheduler()->AddAlarmAtUs(
        timer()->NowUs() + 1, MakeFunction(this, &ApacheFetchTest::FetchDone));
    apache_fetch_->Wait();
    EXPECT_EQ(0, message_handler_.TotalMessages());
  }

  GoogleString SynthesizeWarning(StringPiece message) {
    return StrCat("W[Wed Jan 01 00:00:00 2014] [Warning] [00000] ", message);
  }

  request_rec request_;
  ApacheWriter* apache_writer_;
  RequestHeaders* request_headers_;
  RequestContextPtr request_ctx_;
  std::unique_ptr<ApacheFetch> apache_fetch_;
};

TEST_F(ApacheFetchTest, WaitIproUnbuffered) { WaitIproTest(false); }

TEST_F(ApacheFetchTest, WaitIproUnbufferedWithTest) {
  GoogleString key = http_cache()->CompositeKey(kJsUrl, kCacheFragment);
  delay_cache()->DelayKey(key);
  // This form looks clearer and more concise, and doesn't require a
  // helper method.  However it doesn't compile because of template
  // problems I didn't feel like debugging.
  //
  // Function* alarm = MakeFunction(delay_cache(), &DelayCache::ReleaseKey,key);
  ApacheFetchTest* test = this;
  Function* alarm = MakeFunction(test, &ApacheFetchTest::ReleaseKey, key);
  server_context_->scheduler()->AddAlarmAtUs(timer()->NowUs() + 100, alarm);
  WaitIproTest(false);
}

TEST_F(ApacheFetchTest, WaitIproBuffered) { WaitIproTest(true); }

// Runs a .pagespeed. resource fetch through the streaming path used by
// InstawebHandler::HandleAsPagespeedResource: ResourceFetch::StartWithDriver
// feeding an unbuffered ApacheFetch, with Wait() pumping the driver's
// scheduler sequence on this (the "request") thread.  The HTTP cache
// lookup's response must be delivered through the scheduler sequence
// (CacheCallback in rewrite_driver.cc); ApacheWriter's thread DCHECKs
// verify the contract.
TEST_F(ApacheFetchTest, WaitPagespeedResourceUnbuffered) {
  AddFilter(RewriteOptions::kRewriteCss);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss,
                                "* { display: none; }", 600);

  GoogleString url = Encode(kTestDomain, "cf", "0", "a.css", "css");
  InitFetch(url, HttpStatus::kOK, false /* unbuffered */);
  apache_fetch_->set_handle_error(false);
  apache_fetch_->set_is_proxy(true);  // Suppresses ApacheFetch's nosniff.
  StreamingPagespeedResourceFetch streaming_fetch(&request_,
                                                  apache_fetch_.get());
  GoogleUrl gurl(url);
  ResourceFetch::StartWithDriver(gurl, ResourceFetch::kDontAutoCleanupDriver,
                                 server_context(), rewrite_driver_,
                                 &streaming_fetch);
  apache_fetch_->Wait();

  EXPECT_TRUE(apache_fetch_->status_ok());
  EXPECT_EQ(200, request_.status);
  // The rewritten (minified) body is streamed out with a Content-Length,
  // and the DEFLATE output filter is enabled for the compressible 200.
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos, actions.find("ap_add_output_filter(DEFLATE)"))
      << actions;
  EXPECT_NE(GoogleString::npos, actions.find("ap_set_content_length(15) "))
      << actions;
  EXPECT_NE(GoogleString::npos, actions.find("ap_rwrite(*{display:none})"))
      << actions;
  // StreamingPagespeedResourceFetch strips the X-Page-Speed header that
  // ResourceFetch adds, and is_proxy suppressed ApacheFetch's nosniff.
  GoogleString headers = HeadersOutToString(&request_);
  EXPECT_EQ(GoogleString::npos, headers.find("X-Page-Speed")) << headers;
  EXPECT_EQ(GoogleString::npos, headers.find("X-Content-Type-Options"))
      << headers;
}

// The cache-hit fast path: a second fetch of the same .pagespeed. URL is
// served straight from the HTTP cache.  That delivery arrives via
// rewrite_driver.cc's CacheCallback, which must hand it to the driver's
// scheduler sequence (pumped by Wait() on this thread) instead of calling
// the unbuffered ApacheFetch from the cache's callback context;
// ApacheWriter's request-thread DCHECKs verify the contract.
TEST_F(ApacheFetchTest, WaitPagespeedResourceCacheHitUnbuffered) {
  AddFilter(RewriteOptions::kRewriteCss);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss,
                                "* { display: none; }", 600);
  GoogleString url = Encode(kTestDomain, "cf", "0", "a.css", "css");
  GoogleUrl gurl(url);

  // First fetch reconstructs the resource, writing it to the HTTP cache.
  InitFetch(url, HttpStatus::kOK, false /* unbuffered */);
  apache_fetch_->set_handle_error(false);
  ResourceFetch::StartWithDriver(gurl, ResourceFetch::kDontAutoCleanupDriver,
                                 server_context(), rewrite_driver_,
                                 apache_fetch_.get());
  apache_fetch_->Wait();
  EXPECT_TRUE(apache_fetch_->status_ok());
  EXPECT_EQ(
      0, server_context()->rewrite_stats()->cached_resource_fetches()->Get());
  MockApache::ActionsSinceLastCall();  // Discard the first response's actions.
  apache_fetch_.reset(nullptr);

  // Second fetch of the same URL must be served from the HTTP cache.
  request_rec request2;
  MockApache::PrepareRequest(&request2);
  {
    RequestContextPtr ctx2(RequestContext::NewTestRequestContextWithTimer(
        server_context()->thread_system(), timer()));
    RewriteDriver* driver2 =
        server_context()->NewCustomRewriteDriver(options()->Clone(), ctx2);
    ApacheFetch fetch2(
        url, kDebugInfo, driver2,
        new ApacheWriter(&request2, server_context()->thread_system()),
        new RequestHeaders(), ctx2, options(), message_handler());
    fetch2.set_buffered(false);
    fetch2.set_handle_error(false);
    fetch2.set_is_proxy(true);
    StreamingPagespeedResourceFetch streaming_fetch2(&request2, &fetch2);
    ResourceFetch::StartWithDriver(gurl, ResourceFetch::kDontAutoCleanupDriver,
                                   server_context(), driver2,
                                   &streaming_fetch2);
    fetch2.Wait();

    EXPECT_TRUE(fetch2.status_ok());
    EXPECT_EQ(200, request2.status);
    EXPECT_EQ(
        1, server_context()->rewrite_stats()->cached_resource_fetches()->Get());
    GoogleString actions = MockApache::ActionsSinceLastCall();
    EXPECT_NE(GoogleString::npos, actions.find("ap_set_content_length(15) "))
        << actions;
    EXPECT_NE(GoogleString::npos, actions.find("ap_rwrite(*{display:none})"))
        << actions;
    driver2->Cleanup();
  }
  MockApache::CleanupRequest(&request2);
}

// is_proxy's only effect is suppressing ApacheFetch's own
// X-Content-Type-Options: nosniff header; the streaming .pagespeed.
// resource path relies on that to keep resource responses byte-identical
// with the old buffered path.
TEST_F(ApacheFetchTest, IsProxySuppressesNosniffUnbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kOK);
  apache_fetch_->set_is_proxy(true);
  EXPECT_TRUE(apache_fetch_->Write("hello", message_handler()));
  EXPECT_EQ(200, request_.status);
  EXPECT_STREQ(
      "ap_set_content_type(text/plain) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/plain) "
      "ap_rwrite(hello)",
      MockApache::ActionsSinceLastCall());
  GoogleString headers = HeadersOutToString(&request_);
  EXPECT_EQ(GoogleString::npos, headers.find("X-Content-Type-Options"))
      << headers;
  apache_fetch_->Done(true);
}

TEST_F(ApacheFetchTest, SuccessBuffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kOK);
  EXPECT_TRUE(apache_fetch_->Write("hello ", &message_handler_));
  EXPECT_TRUE(apache_fetch_->Write("world", &message_handler_));
  EXPECT_TRUE(apache_fetch_->Flush(&message_handler_));
  EXPECT_TRUE(apache_fetch_->Write(".", &message_handler_));
  EXPECT_TRUE(apache_fetch_->Flush(&message_handler_));
  // All flushes are dropped, all writed are buffered.
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_EQ(200, request_.status);
  EXPECT_EQ(
      "ap_set_content_type(text/plain) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/plain) "
      "ap_rwrite(hello world.)",  // writes combined into one call.
      MockApache::ActionsSinceLastCall());

  // TODO(jefftk): Cookies are present here even though we asked ApacheWriter to
  // remove them because of an ApacheWriter bug.
  EXPECT_EQ(kExpectedHeaders, HeadersOutToString(&request_));
}

TEST_F(ApacheFetchTest, SuccessUnbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kOK);
  EXPECT_TRUE(apache_fetch_->Write("hello ", &message_handler_));
  EXPECT_EQ(200, request_.status);
  EXPECT_STREQ(
      "ap_set_content_type(text/plain) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/plain) "
      "ap_rwrite(hello )",
      MockApache::ActionsSinceLastCall());

  // TODO(jefftk): Cookies are present here even though we asked ApacheWriter to
  // remove them because of an ApacheWriter bug.
  EXPECT_EQ(kExpectedHeaders, HeadersOutToString(&request_));

  EXPECT_TRUE(apache_fetch_->Write("world", message_handler()));
  EXPECT_STREQ("ap_rwrite(world)", MockApache::ActionsSinceLastCall());
  EXPECT_TRUE(apache_fetch_->Flush(message_handler()));
  EXPECT_STREQ("ap_rflush()", MockApache::ActionsSinceLastCall());
  EXPECT_TRUE(apache_fetch_->Write(".", message_handler()));
  EXPECT_STREQ("ap_rwrite(.)", MockApache::ActionsSinceLastCall());
  EXPECT_TRUE(apache_fetch_->Flush(message_handler()));
  EXPECT_STREQ("ap_rflush()", MockApache::ActionsSinceLastCall());
  apache_fetch_->Done(true);
}

TEST_F(ApacheFetchTest, NotFound404Buffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kNotFound);
  EXPECT_TRUE(apache_fetch_->Write("Couldn't find it.", &message_handler_));
  // Writes are buffered.
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_EQ(404, request_.status);
  EXPECT_EQ(kExpectedHeaders, HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_set_content_type(text/plain) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/plain) "
      "ap_rwrite(Couldn't find it.)",
      MockApache::ActionsSinceLastCall());
}

TEST_F(ApacheFetchTest, NotFound404Unbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kNotFound);
  EXPECT_TRUE(apache_fetch_->Write("Couldn't find it.", message_handler()));
  EXPECT_EQ(404, request_.status);
  EXPECT_EQ(kExpectedHeaders, HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_set_content_type(text/plain) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/plain) "
      "ap_rwrite(Couldn't find it.)",
      MockApache::ActionsSinceLastCall());
  apache_fetch_->Done(true);
}

// With handle_error disabled the caller answers error responses itself
// (e.g. the .pagespeed. resource path sends its own 404 after Wait());
// ApacheFetch must not emit the failed fetch's headers or body.
TEST_F(ApacheFetchTest, NotFound404SuppressedUnbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kNotFound);
  apache_fetch_->set_handle_error(false);
  EXPECT_TRUE(apache_fetch_->Write("Couldn't find it.", message_handler()));
  EXPECT_TRUE(apache_fetch_->Flush(message_handler()));
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());
  EXPECT_STREQ("", HeadersOutToString(&request_));
  EXPECT_FALSE(apache_fetch_->status_ok());
  apache_fetch_->Done(true);
}

TEST_F(ApacheFetchTest, NotFound404SuppressedBuffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kNotFound);
  apache_fetch_->set_handle_error(false);
  EXPECT_TRUE(apache_fetch_->Write("Couldn't find it.", &message_handler_));
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());
  EXPECT_STREQ("", HeadersOutToString(&request_));
  EXPECT_FALSE(apache_fetch_->status_ok());
}

TEST_F(ApacheFetchTest, NoContentType200Buffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kOK);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll(
      HttpAttributes::kContentType));
  EXPECT_TRUE(apache_fetch_->Write("Example response.", &message_handler_));
  // Writes are buffered.
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_EQ(403, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Content-Type: text/html\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      "X-Content-Type-Options: nosniff\n"
      "Cache-Control: max-age=0, no-cache\n",
      HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_set_content_type(text/html) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/html) "
      "ap_rwrite(Missing Content-Type required for proxied resource)",
      MockApache::ActionsSinceLastCall());
}

TEST_F(ApacheFetchTest, NoContentType200Unbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kOK);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll(
      HttpAttributes::kContentType));
  EXPECT_TRUE(apache_fetch_->Write("Example response.", message_handler()));
  EXPECT_EQ(403, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Content-Type: text/html\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      "X-Content-Type-Options: nosniff\n"
      "Cache-Control: max-age=0, no-cache\n",
      HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_set_content_type(text/html) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/html) "
      "ap_rwrite(Missing Content-Type required for proxied resource)",
      MockApache::ActionsSinceLastCall());
  apache_fetch_->Done(true);
}

TEST_F(ApacheFetchTest, NoContentType301Buffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kMovedPermanently);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll("Content-Type"));
  apache_fetch_->response_headers()->Add("Location", "elsewhere");

  EXPECT_TRUE(apache_fetch_->Write("moved elsewhere", &message_handler_));
  // Writes are buffered.
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_EQ(403, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Location: elsewhere\n"
      "Content-Type: text/html\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      "X-Content-Type-Options: nosniff\n"
      "Cache-Control: max-age=0, no-cache\n",
      HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_set_content_type(text/html) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/html) "
      "ap_rwrite(Missing Content-Type required for proxied resource)",
      MockApache::ActionsSinceLastCall());
}

TEST_F(ApacheFetchTest, NoContentType301Unbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kMovedPermanently);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll("Content-Type"));
  apache_fetch_->response_headers()->Add("Location", "elsewhere");
  EXPECT_TRUE(apache_fetch_->Write("moved elsewhere", message_handler()));
  EXPECT_EQ(403, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Location: elsewhere\n"
      "Content-Type: text/html\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      "X-Content-Type-Options: nosniff\n"
      "Cache-Control: max-age=0, no-cache\n",
      HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_set_content_type(text/html) "
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_set_content_type(text/html) "
      "ap_rwrite(Missing Content-Type required for proxied resource)",
      MockApache::ActionsSinceLastCall());
  apache_fetch_->Done(true);
}

TEST_F(ApacheFetchTest, NoContentType304Buffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kNotModified);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll("Content-Type"));

  EXPECT_TRUE(apache_fetch_->Write("not modified", &message_handler_));
  // Writes are buffered.
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_EQ(304, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      // No default Cache-Control on a 304: it would make clients update
      // their stored response's caching (RFC 7232 section 4.1).
      "X-Content-Type-Options: nosniff\n",
      HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_rwrite(not modified)",
      MockApache::ActionsSinceLastCall());
}

TEST_F(ApacheFetchTest, NoContentType304Unbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kNotModified);
  apache_fetch_->response_headers()->set_status_code(304);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll("Content-Type"));
  EXPECT_TRUE(apache_fetch_->Write("not modified", message_handler()));
  EXPECT_EQ(304, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      // No default Cache-Control on a 304: it would make clients update
      // their stored response's caching (RFC 7232 section 4.1).
      "X-Content-Type-Options: nosniff\n",
      HeadersOutToString(&request_));
  EXPECT_EQ(
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT) "
      "ap_rwrite(not modified)",
      MockApache::ActionsSinceLastCall());
  apache_fetch_->Done(true);
}

TEST_F(ApacheFetchTest, NoContentType204Buffered) {
  InitFetchBuffered(kExampleUrl, HttpStatus::kNoContent);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll("Content-Type"));
  apache_fetch_->HeadersComplete();
  // Headers are buffered.
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();
  EXPECT_EQ(204, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      // Like 304s, 204s are exempt from the default Cache-Control.
      "X-Content-Type-Options: nosniff\n",
      HeadersOutToString(&request_));
  EXPECT_STREQ(
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT)",
      MockApache::ActionsSinceLastCall());
}

TEST_F(ApacheFetchTest, NoContentType204Unbuffered) {
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kNoContent);
  apache_fetch_->response_headers()->set_status_code(204);
  EXPECT_TRUE(apache_fetch_->response_headers()->RemoveAll("Content-Type"));
  apache_fetch_->HeadersComplete();
  EXPECT_EQ(204, request_.status);
  EXPECT_STREQ(
      "Set-Cookie: test=cookie\n"
      "Set-Cookie: tasty=cookie\n"
      "Set-Cookie2: obselete\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\n"
      // Like 304s, 204s are exempt from the default Cache-Control.
      "X-Content-Type-Options: nosniff\n",
      HeadersOutToString(&request_));
  EXPECT_STREQ(
      "ap_remove_output_filter(MOD_EXPIRES) "
      "ap_remove_output_filter(FIXUP_HEADERS_OUT)",
      MockApache::ActionsSinceLastCall());
  apache_fetch_->Done(true);
}

// ---------------------------------------------------------------------------
// Zero-copy ALIASED serve (CycloneZeroCopyServe, the design record) -- the Apache
// WriteMapped sink.  The Cyclone lease is scripted through the
// MappedSharedString hooks; MockApache's functional ap_pass_brigade reads
// every bucket (running the PAGESPEED_MMAP read barrier) and logs the
// bytes, so "aliased brigade serve" vs "classic ap_rwrite copy" is
// directly observable in the action log.

namespace {

struct LeaseHookScript {
  std::vector<LeaseRenewal> strict_results{LeaseRenewal::kOk};
  size_t strict_calls = 0;
  uint64_t ns_until_forced_wrap = ~static_cast<uint64_t>(0);
  int released = 0;
};

void LeaseScriptRelease(void* user_data) {
  static_cast<LeaseHookScript*>(user_data)->released++;
}
int LeaseScriptRenew(void* user_data) { return 1; }
int LeaseScriptRenewStrict(void* user_data) {
  LeaseHookScript* script = static_cast<LeaseHookScript*>(user_data);
  size_t i = script->strict_calls;
  if (i >= script->strict_results.size()) {
    i = script->strict_results.size() - 1;
  }
  script->strict_calls++;
  return static_cast<int>(script->strict_results[i]);
}
uint64_t LeaseScriptNsUntilForcedWrap(void* user_data) {
  return static_cast<LeaseHookScript*>(user_data)->ns_until_forced_wrap;
}

const char kMappedBody[] = "MAPPED-BODY-BYTES served without a copy";

}  // namespace

class ApacheFetchZeroCopyTest : public ApacheFetchTest {
 public:
  ApacheFetchZeroCopyTest() {
    // Refcounted static-property init, mirroring RewriteOptionsTestBase;
    // needed to construct SystemRewriteOptions instances below.
    SystemRewriteOptions::Initialize();
  }
  ~ApacheFetchZeroCopyTest() override { SystemRewriteOptions::Terminate(); }

 protected:
  // An options object carrying the Apache opt-in: CycloneZeroCopyServe
  // explicitly set on (value alone is not enough; the sink requires
  // was_set).
  SystemRewriteOptions* NewOptedInOptions() {
    SystemRewriteOptions* system_options = new SystemRewriteOptions(
        "zerocopy-test", server_context()->thread_system());
    system_options->set_cyclone_zero_copy_serve(true);
    options_holder_.reset(system_options);
    return system_options;
  }

  void InitFetchWithOptions(const RewriteOptions* opts) {
    apache_fetch_ = std::make_unique<ApacheFetch>(
        kExampleUrl, kDebugInfo, rewrite_driver_, apache_writer_,
        request_headers_, request_ctx_, opts, message_handler());
    apache_fetch_->set_buffered(false);
    apache_fetch_->set_is_proxy(true);
    apache_fetch_->response_headers()->SetStatusAndReason(HttpStatus::kOK);
    apache_fetch_->response_headers()->set_major_version(1);
    apache_fetch_->response_headers()->set_minor_version(1);
    apache_fetch_->response_headers()->MergeContentType("image/jpeg");
  }

  MappedSharedString MakePin() {
    return MappedSharedString::FromMappedView(
        region_, STATIC_STRLEN(kMappedBody), LeaseScriptRelease,
        LeaseScriptRenew, LeaseScriptRenewStrict, LeaseScriptNsUntilForcedWrap,
        &script_);
  }

  StringPiece MappedSpan() {
    return StringPiece(region_, STATIC_STRLEN(kMappedBody));
  }

  // Replaces the mock's fake filter chain with the given output-filter
  // chain and gives the request a connection with a bucket allocator, so
  // ApacheWriter::RequestServesBodyVerbatim() can walk the chain and an
  // aliased brigade can be built and passed.
  void SetUpServeChain(const char* const* names, size_t count) {
    ap_filter_t** filter = &request_.output_filters;
    for (size_t i = 0; i < count; ++i, filter = &(*filter)->next) {
      *filter = static_cast<ap_filter_t*>(
          apr_palloc(request_.pool, sizeof(ap_filter_t)));
      (*filter)->frec = static_cast<ap_filter_rec_t*>(
          apr_palloc(request_.pool, sizeof(ap_filter_rec_t)));
      (*filter)->frec->name = names[i];
    }
    *filter = nullptr;
    conn_ =
        static_cast<conn_rec*>(apr_pcalloc(request_.pool, sizeof(conn_rec)));
    conn_->bucket_alloc = apr_bucket_alloc_create(request_.pool);
    request_.connection = conn_;
    request_.main = nullptr;
    request_.prev = nullptr;
    request_.next = nullptr;
    request_.header_only = 0;
  }

  // The stock verbatim serve chain.
  void SetUpVerbatimServeChain() {
    static const char* const kNames[] = {"content_length", "http_header",
                                         "core"};
    SetUpServeChain(kNames, arraysize(kNames));
  }

  int64 StatValue(const char* name) {
    Variable* var = statistics()->FindVariable(name);
    return var == nullptr ? -1 : var->Get();
  }

  std::unique_ptr<RewriteOptions> options_holder_;
  LeaseHookScript script_;
  conn_rec* conn_ = nullptr;
  char region_[STATIC_STRLEN(kMappedBody) + 1] = {0};
};

// Without the explicit opt-in (options are not SystemRewriteOptions, or
// CycloneZeroCopyServe was never set in the config), mapped bytes are
// served through the classic copying path -- and still verified after the
// copy.
TEST_F(ApacheFetchZeroCopyTest, NoOptInServesVerifiedCopy) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  InitFetchUnbuffered(kExampleUrl, HttpStatus::kOK);
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos,
            actions.find(StrCat("ap_rwrite(", kMappedBody, ")")))
      << actions;
  EXPECT_EQ(GoogleString::npos, actions.find("ap_pass_brigade")) << actions;
  // The copied-out counter tracks degradation of alias-ELIGIBLE serves
  // only; the not-opted-in verified copy is the classic serve and counts
  // nowhere -- including zerocopy_serve_ineligible, which is scoped to
  // OPTED-IN serves (no opt-in, nothing to diagnose).
  EXPECT_EQ(0, StatValue("zerocopy_serve_copied_out"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_ineligible"));
  // Copy-then-verify ran exactly once.
  EXPECT_EQ(1u, script_.strict_calls);
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  apache_fetch_->Done(true);
}

// With the explicit opt-in and a verbatim chain, the body goes out as an
// aliased brigade: no ap_rwrite, bytes read straight from the mapped
// region under the read barrier.
TEST_F(ApacheFetchZeroCopyTest, OptInAliasesVerbatimServe) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  InitFetchWithOptions(NewOptedInOptions());
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos,
            actions.find(StrCat("ap_pass_brigade(", kMappedBody, ")")))
      << actions;
  EXPECT_EQ(GoogleString::npos, actions.find("ap_rwrite")) << actions;
  EXPECT_EQ(1, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_copied_out"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_ineligible"));
  // Emit-time renewal plus the bucket's per-send read barrier.
  EXPECT_EQ(2u, script_.strict_calls);
  // The bucket was consumed by the pass; its pin reference is gone.
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  apache_fetch_->Done(true);
}

// mod_reqtimeout's output filter (default-enabled on Debian/Ubuntu) is
// timeout bookkeeping that passes brigades through untouched; its presence
// on the connection chain must not disqualify aliasing (before
// it was allowlisted, aliasing was dead on every stock install).
TEST_F(ApacheFetchZeroCopyTest, OptInAliasesWithReqtimeoutInChain) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  static const char* const kNames[] = {"content_length", "http_header",
                                       "http_outerror", "reqtimeout", "core"};
  SetUpServeChain(kNames, arraysize(kNames));
  InitFetchWithOptions(NewOptedInOptions());
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos,
            actions.find(StrCat("ap_pass_brigade(", kMappedBody, ")")))
      << actions;
  EXPECT_EQ(GoogleString::npos, actions.find("ap_rwrite")) << actions;
  EXPECT_EQ(1, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_ineligible"));
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  apache_fetch_->Done(true);
}

// Opt-in, but a non-verbatim filter (mod_deflate) sits on the chain: the
// serve must degrade to the verified copy.
TEST_F(ApacheFetchZeroCopyTest, DeflateOnChainForcesCopy) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  // Splice a deflate filter in front, as ap_add_output_filter would.
  ap_filter_t* deflate =
      static_cast<ap_filter_t*>(apr_palloc(request_.pool, sizeof(ap_filter_t)));
  deflate->frec = static_cast<ap_filter_rec_t*>(
      apr_palloc(request_.pool, sizeof(ap_filter_rec_t)));
  deflate->frec->name = "deflate";
  deflate->next = request_.output_filters;
  request_.output_filters = deflate;
  InitFetchWithOptions(NewOptedInOptions());
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos,
            actions.find(StrCat("ap_rwrite(", kMappedBody, ")")))
      << actions;
  // The settle pass ran before the (failed) walk: real mod_filter
  // harnesses would have dispatched and, unmatched, left the chain.
  EXPECT_NE(GoogleString::npos, actions.find("ap_pass_brigade(EMPTY)"))
      << actions;
  EXPECT_EQ(0, StatValue("zerocopy_serve_aliased"));
  // A non-verbatim chain is not alias-ELIGIBLE, so its verified copy is the
  // classic serve, not a counted degrade...
  EXPECT_EQ(0, StatValue("zerocopy_serve_copied_out"));
  // ...but the operator opted in and is not getting aliasing: that IS the
  // ineligible signal.
  EXPECT_EQ(1, StatValue("zerocopy_serve_ineligible"));
  apache_fetch_->Done(true);
}

// Opt-in, but the lease reports a wrap in flight at emit: degrade to the
// verified copy (no reset).
TEST_F(ApacheFetchZeroCopyTest, WrapInFlightAtEmitForcesCopy) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  InitFetchWithOptions(NewOptedInOptions());
  script_.strict_results = {LeaseRenewal::kCopyNow, LeaseRenewal::kOk};
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos,
            actions.find(StrCat("ap_rwrite(", kMappedBody, ")")))
      << actions;
  EXPECT_EQ(0, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(1, StatValue("zerocopy_serve_copied_out"));
  apache_fetch_->Done(true);
}

// A torn borrow at emit fails the serve closed: no body bytes reach the
// writer, the fetch reports failure, and the connection is aborted so the
// committed Content-Length can never pass off a corrupt body as complete.
TEST_F(ApacheFetchZeroCopyTest, TornAtEmitFailsClosedAndAborts) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  InitFetchWithOptions(NewOptedInOptions());
  script_.strict_results = {LeaseRenewal::kTorn};
  MappedSharedString pin = MakePin();
  EXPECT_FALSE(
      apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_EQ(GoogleString::npos, actions.find("ap_rwrite")) << actions;
  // The settle pass (empty brigade) may appear in the log, but no BODY
  // bytes may reach the chain.
  EXPECT_EQ(GoogleString::npos, actions.find(kMappedBody)) << actions;
  EXPECT_EQ(1, conn_->aborted);
  EXPECT_EQ(1, StatValue("zerocopy_serve_renew_fail_reset"));
  apache_fetch_->Done(false);
}

// Explicitly-unset on a REAL SystemRewriteOptions whose compiled default is
// true: the Apache sink must still refuse to alias (was_set gate).
TEST_F(ApacheFetchZeroCopyTest, DefaultOnWithoutExplicitSetServesCopy) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  SystemRewriteOptions* system_options = new SystemRewriteOptions(
      "zerocopy-default-test", server_context()->thread_system());
  ASSERT_TRUE(system_options->cyclone_zero_copy_serve());  // Default value.
  ASSERT_FALSE(system_options->has_cyclone_zero_copy_serve());
  options_holder_.reset(system_options);
  InitFetchWithOptions(system_options);
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos,
            actions.find(StrCat("ap_rwrite(", kMappedBody, ")")))
      << actions;
  EXPECT_EQ(GoogleString::npos, actions.find("ap_pass_brigade")) << actions;
  EXPECT_EQ(0, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_copied_out"));
  // was_set gate failed => not opted in => not an ineligible-serve either.
  EXPECT_EQ(0, StatValue("zerocopy_serve_ineligible"));
  apache_fetch_->Done(true);
}

// Slow-client geometry: the downstream chain accepts half the aliased
// bucket, parks the rest (setaside => verified copy-out), and drains it on
// the next send attempt after a fresh read.  The body must arrive
// byte-identical, with the mid-serve degrade counted.
TEST_F(ApacheFetchZeroCopyTest, PartialDrainCopiesOutParkedTail) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  MockApache::set_partial_pass_brigade(true);
  InitFetchWithOptions(NewOptedInOptions());
  MappedSharedString pin = MakePin();
  EXPECT_TRUE(apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  const size_t half = STATIC_STRLEN(kMappedBody) / 2;
  GoogleString expected = StrCat(
      "ap_pass_brigade_partial(first=", StringPiece(kMappedBody, half),
      ",setaside=0,rest=",
      StringPiece(kMappedBody + half, STATIC_STRLEN(kMappedBody) - half), ")");
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos, actions.find(expected)) << actions;
  EXPECT_EQ(1, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(1, StatValue("zerocopy_serve_copied_out"));
  EXPECT_EQ(0, StatValue("zerocopy_serve_renew_fail_reset"));
  // The parked tail's copy-out released the pin at setaside time.
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  apache_fetch_->Done(true);
}

// Torn mid-send: emit and the first send barrier pass, then the wrap
// commits while the tail is being parked for a slow client.  The parked
// bucket is poisoned, the next send attempt's read fails, and the serve
// aborts the connection -- never a complete-but-corrupt body.
TEST_F(ApacheFetchZeroCopyTest, TornMidSendAbortsConnection) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  SetUpVerbatimServeChain();
  MockApache::set_partial_pass_brigade(true);
  InitFetchWithOptions(NewOptedInOptions());
  // {emit barrier, first-send read barrier, setaside verify} in order.
  script_.strict_results = {LeaseRenewal::kOk, LeaseRenewal::kOk,
                            LeaseRenewal::kTorn};
  MappedSharedString pin = MakePin();
  EXPECT_FALSE(
      apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  GoogleString actions = MockApache::ActionsSinceLastCall();
  EXPECT_NE(GoogleString::npos, actions.find("rest=READ_ERROR")) << actions;
  EXPECT_EQ(1, conn_->aborted);
  EXPECT_EQ(1, StatValue("zerocopy_serve_aliased"));
  EXPECT_EQ(1, StatValue("zerocopy_serve_renew_fail_reset"));
  EXPECT_EQ(3u, script_.strict_calls);
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  apache_fetch_->Done(false);
}

// A torn borrow in BUFFERED mode (headers not yet committed) must poison
// the response: Wait() emits a 500, never the promised clean 200 with a
// missing body.
TEST_F(ApacheFetchZeroCopyTest, BufferedTornPoisonsResponse) {
  memcpy(region_, kMappedBody, STATIC_STRLEN(kMappedBody));
  InitFetchBuffered(kExampleUrl, HttpStatus::kOK);
  script_.strict_results = {LeaseRenewal::kTorn};
  MappedSharedString pin = MakePin();
  EXPECT_FALSE(
      apache_fetch_->WriteMapped(MappedSpan(), pin, message_handler()));
  // Nothing reached the writer yet (buffered).
  EXPECT_STREQ("", MockApache::ActionsSinceLastCall());

  WaitExpectSuccess();

  EXPECT_EQ(500, request_.status);
  EXPECT_FALSE(apache_fetch_->status_ok());
  GoogleString actions = MockApache::ActionsSinceLastCall();
  // No body bytes -- and in particular not the torn mapped bytes.
  EXPECT_EQ(GoogleString::npos, actions.find("ap_rwrite")) << actions;
}

}  // namespace net_instaweb

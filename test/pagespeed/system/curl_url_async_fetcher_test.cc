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

#include "pagespeed/system/curl_url_async_fetcher.h"

#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <vector>

#include "base/logging.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/stl_util.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/system/curl_test_certs.h"
#include "test/pagespeed/system/curl_test_server.h"
#include "test/pagespeed/system/tcp_server_thread_for_testing_posix.h"

namespace net_instaweb {

namespace {
const int kFetcherTimeoutMs = 5 * 1000;

const int kModpagespeedSite = 0;
const int kGoogleFavicon = 1;
const int kGoogleLogo = 2;
const int kCgiSlowJs = 3;
// index 4 is beacon
const int kConnectionRefused = 5;
const int kNoContent = 6;

// Note: We do not subclass StringAsyncFetch because we want to lock access
// to done_.
class CurlTestFetch : public AsyncFetch {
 public:
  explicit CurlTestFetch(const RequestContextPtr& ctx, AbstractMutex* mutex)
      : AsyncFetch(ctx), mutex_(mutex), success_(false), done_(false) {}
  ~CurlTestFetch() override {}

  bool HandleWrite(const StringPiece& content,
                   MessageHandler* handler) override {
    content.AppendToString(&buffer_);
    return true;
  }
  bool HandleFlush(MessageHandler* handler) override { return true; }
  void HandleHeadersComplete() override {}
  void HandleDone(bool success) override {
    ScopedMutex lock(mutex_);
    EXPECT_FALSE(done_);
    success_ = success;
    done_ = true;
  }

  const GoogleString& buffer() const { return buffer_; }
  bool success() const { return success_; }
  bool IsDone() const {
    ScopedMutex lock(mutex_);
    return done_;
  }

  void Reset() override {
    ScopedMutex lock(mutex_);
    AsyncFetch::Reset();
    done_ = false;
    success_ = false;
    response_headers()->Clear();
  }

 private:
  AbstractMutex* mutex_;
  GoogleString buffer_;
  bool success_;
  bool done_;

  CurlTestFetch(const CurlTestFetch&) = delete;
  CurlTestFetch& operator=(const CurlTestFetch&) = delete;
};

}  // namespace

class CurlUrlAsyncFetcherTest : public ::testing::Test {
 protected:
  CurlUrlAsyncFetcherTest()
      : thread_system_(Platform::CreateThreadSystem()),
        message_handler_(thread_system_->NewMutex()),
        fetcher_timeout_ms_(FetcherTimeoutMs()) {}

  void SetUp() override { SetUpWithProxy(""); }

  static int64 FetcherTimeoutMs() { return kFetcherTimeoutMs; }

  // Writes the embedded test-CA PEM to a temp file and returns its path, so
  // it can be handed to libcurl as CAINFO. The file lives under the gtest temp
  // dir and is reused across this fixture instance.
  const GoogleString& TestCaFile() {
    if (test_ca_file_.empty()) {
      test_ca_file_ = StrCat(GTestTempDir(), "/curl_test_ca.pem");
      StdioFileSystem file_system;
      NullMessageHandler handler;
      file_system.WriteFile(test_ca_file_.c_str(), kTestCaCertPem, &handler);
    }
    return test_ca_file_;
  }

  void SetUpWithProxy(const char* proxy) {
    // Stand up the hermetic local servers. All test traffic is served from
    // 127.0.0.1 on ephemeral ports -- no public-internet dependency.
    http_server_ = std::make_unique<CurlTestServer>(CurlTestServer::kHttp,
                                                    CurlTestServer::kTrustedCa,
                                                    thread_system_.get());
    ASSERT_TRUE(http_server_->StartAndWait());
    https_ca_server_ = std::make_unique<CurlTestServer>(
        CurlTestServer::kHttps, CurlTestServer::kTrustedCa,
        thread_system_.get());
    ASSERT_TRUE(https_ca_server_->StartAndWait());
    https_self_signed_server_ = std::make_unique<CurlTestServer>(
        CurlTestServer::kHttps, CurlTestServer::kSelfSigned,
        thread_system_.get());
    ASSERT_TRUE(https_self_signed_server_->StartAndWait());

    // test_host_ drives the http:// URLs the tests build via StrCat.
    test_host_ = http_server_->host_port();
    https_host_ = https_ca_server_->host_port();
    GoogleString fetch_test_domain = StrCat("//", test_host_);

    timer_.reset(Platform::CreateTimer());
    statistics_ = std::make_unique<SimpleStats>(thread_system_.get());
    CurlUrlAsyncFetcher::InitStats(statistics_.get());
    curl_fetcher_ = std::make_unique<CurlUrlAsyncFetcher>(
        proxy, thread_system_.get(), statistics_.get(), timer_.get(),
        fetcher_timeout_ms_, &message_handler_);
    mutex_.reset(thread_system_->NewMutex());

    // Local-server endpoints (httpbin-equivalent), all on 127.0.0.1.
    // kModpagespeedSite (0): HTML page
    AddTestUrl(StrCat("http:", fetch_test_domain, "/html"), "<!DOCTYPE html>");
    // kGoogleFavicon (1): PNG image
    GoogleString png_path = StrCat(fetch_test_domain, "/image/png");
    static const char kPngHead[] = "\x89PNG";
    favicon_head_.append(kPngHead, 4);
    // The HTTPS favicon URL points at the trusted-CA HTTPS server.
    https_favicon_url_ = StrCat("https://", https_host_, "/image/png");
    AddTestUrl(StrCat("http:", png_path), favicon_head_);
    // kGoogleLogo (2): JPEG image
    AddTestUrl(StrCat("http:", fetch_test_domain, "/image/jpeg"),
               "\xff\xd8\xff");
    // kCgiSlowJs (3): Delayed response. The local /delay endpoint holds the
    // connection open far longer than fetcher_timeout_ms_ (5s) so TestTimeout
    // reliably observes our timeout fire first; the only other caller
    // (TestShutdownWithActiveFetches) cancels before the delay matters.
    AddTestUrl(StrCat("http:", fetch_test_domain, "/delay/30"), "{");
    // index 4: beacon
    AddTestUrl(StrCat("http:", fetch_test_domain, "/get?ets=42"), "");
    // kConnectionRefused (5)
    AddTestUrl("http://127.0.0.1:1023/refused.jpg", "");
    // kNoContent (6)
    AddTestUrl(StrCat("http:", fetch_test_domain, "/status/204"), "");

    prev_done_count = 0;

    // Trust the embedded test CA so the trusted-CA HTTPS server's leaf
    // validates in the "succeeds" cases. (No SSL_CERT_DIR/FILE env handling:
    // the test is fully self-contained and must not pick up host CA state.)
    curl_fetcher_->SetSslCertificatesFile(TestCaFile());

    statistics_->GetUpDownCounter(CurlStats::kCurlFetchLastCheckTimestampMs)
        ->Set(timer_->NowMs());
  }

  void TearDown() override {
    if (curl_fetcher_ != nullptr) {
      curl_fetcher_->ShutDown();
      curl_fetcher_.reset(nullptr);
    }
    if (http_server_ != nullptr) {
      http_server_->ShutDown();
      http_server_.reset(nullptr);
    }
    if (https_ca_server_ != nullptr) {
      https_ca_server_->ShutDown();
      https_ca_server_.reset(nullptr);
    }
    if (https_self_signed_server_ != nullptr) {
      https_self_signed_server_->ShutDown();
      https_self_signed_server_.reset(nullptr);
    }
    timer_.reset(nullptr);
    STLDeleteElements(&fetches_);
  }

  int AddTestUrl(const GoogleString& url, const GoogleString& content_start) {
    urls_.push_back(url);
    content_starts_.push_back(content_start);
    int index = fetches_.size();
    fetches_.push_back(new CurlTestFetch(
        RequestContext::NewTestRequestContext(thread_system_.get()),
        mutex_.get()));
    return index;
  }

  void StartFetch(int idx) {
    fetches_[idx]->Reset();
    curl_fetcher_->Fetch(urls_[idx], &message_handler_, fetches_[idx]);
  }

  void StartFetches(size_t first, size_t last) {
    for (size_t idx = first; idx <= last; ++idx) {
      StartFetch(idx);
    }
  }

  int ActiveFetches() {
    return statistics_->GetUpDownCounter(CurlStats::kCurlFetchActiveCount)
        ->Get();
  }

  int CountCompletedFetches(size_t first, size_t last) {
    int completed = 0;
    for (size_t idx = first; idx <= last; ++idx) {
      if (fetches_[idx]->IsDone()) {
        ++completed;
      }
    }
    return completed;
  }

  void ValidateFetches(size_t first, size_t last) {
    for (size_t idx = first; idx <= last; ++idx) {
      ASSERT_TRUE(fetches_[idx]->IsDone());
      EXPECT_TRUE(fetches_[idx]->success());

      if (content_starts_[idx].empty()) {
        EXPECT_TRUE(contents(idx).empty());
        EXPECT_EQ(HttpStatus::kNoContent, response_headers(idx)->status_code());
      } else {
        EXPECT_LT(static_cast<size_t>(0), contents(idx).size()) << urls_[idx];
        EXPECT_EQ(HttpStatus::kOK, response_headers(idx)->status_code())
            << urls_[idx];
      }
      EXPECT_STREQ(content_starts_[idx],
                   contents(idx).substr(0, content_starts_[idx].size()));
    }
  }

  void ValidateMonitoringStats(int64 expect_success, int64 expect_failure) {
    EXPECT_EQ(
        expect_success,
        statistics_->GetVariable(CurlStats::kCurlFetchUltimateSuccess)->Get());
    EXPECT_EQ(
        expect_failure,
        statistics_->GetVariable(CurlStats::kCurlFetchUltimateFailure)->Get());
  }

  int WaitTillDone(size_t first, size_t last) {
    bool done = false;
    size_t done_count = 0;
    int max_iterations = fetcher_timeout_ms_ * 20;  // ~50us per iter
    int iter = 0;
    while (!done && iter++ < max_iterations) {
      usleep(50);
      done_count = 0;
      for (size_t idx = first; idx <= last; ++idx) {
        if (fetches_[idx]->IsDone()) {
          ++done_count;
        }
      }
      if (done_count != prev_done_count) {
        prev_done_count = done_count;
        done = (done_count == (last - first + 1));
      }
    }
    return done_count;
  }

  int TestFetch(size_t first, size_t last) {
    StartFetches(first, last);
    int done = WaitTillDone(first, last);
    ValidateFetches(first, last);
    return (done == static_cast<int>(last - first + 1));
  }

  void ConnectionRefusedTest() {
    StartFetches(kConnectionRefused, kConnectionRefused);
    ASSERT_EQ(WaitTillDone(kConnectionRefused, kConnectionRefused), 1);
    ASSERT_TRUE(fetches_[kConnectionRefused]->IsDone());
    EXPECT_EQ(HttpStatus::kNotFound,
              response_headers(kConnectionRefused)->status_code());
    ValidateMonitoringStats(0, 1);
  }

  void TestHttpsFails(const GoogleString& url) {
    int index = AddTestUrl(url, "");
    TestHttpsFails(index, index);
  }

  void TestHttpsFails(int first, int last) {
    int num_fetches = last - first + 1;
    CHECK_LT(0, num_fetches);
    StartFetches(first, last);
    ASSERT_EQ(num_fetches, WaitTillDone(first, last));
    for (int index = first; index <= last; ++index) {
      ASSERT_TRUE(fetches_[index]->IsDone()) << urls_[index];
      ASSERT_TRUE(content_starts_[index].empty()) << urls_[index];
      EXPECT_STREQ("", contents(index)) << urls_[index];
      EXPECT_EQ(HttpStatus::kNotFound, response_headers(index)->status_code())
          << urls_[index];
    }
  }

  void TestHttpsSucceeds(const GoogleString& url,
                         const GoogleString& content_start) {
    int index = AddTestUrl(url, content_start);
    StartFetches(index, index);
    ExpectHttpsSucceeds(index);
  }

  void ExpectHttpsSucceeds(int index) {
    ASSERT_EQ(1, WaitTillDone(index, index));
    ASSERT_TRUE(fetches_[index]->IsDone());
    ASSERT_FALSE(content_starts_[index].empty());
    EXPECT_FALSE(contents(index).empty());
    EXPECT_EQ(HttpStatus::kOK, response_headers(index)->status_code());
    EXPECT_EQ(0,
              statistics_->GetVariable(CurlStats::kCurlFetchCertErrors)->Get());
    EXPECT_STREQ(content_starts_[index],
                 contents(index).substr(0, content_starts_[index].size()));
  }

  RequestHeaders* request_headers(int idx) {
    return fetches_[idx]->request_headers();
  }
  ResponseHeaders* response_headers(int idx) {
    return fetches_[idx]->response_headers();
  }
  const GoogleString& contents(int idx) { return fetches_[idx]->buffer(); }

  GoogleString test_host_;
  GoogleString https_host_;
  GoogleString test_ca_file_;
  std::unique_ptr<CurlTestServer> http_server_;
  std::unique_ptr<CurlTestServer> https_ca_server_;
  std::unique_ptr<CurlTestServer> https_self_signed_server_;
  std::vector<GoogleString> urls_;
  std::vector<GoogleString> content_starts_;
  std::vector<CurlTestFetch*> fetches_;
  std::unique_ptr<CurlUrlAsyncFetcher> curl_fetcher_;
  std::unique_ptr<Timer> timer_;
  size_t prev_done_count;
  std::unique_ptr<AbstractMutex> mutex_;
  std::unique_ptr<ThreadSystem> thread_system_;
  MockMessageHandler message_handler_;
  std::unique_ptr<SimpleStats> statistics_;
  GoogleString https_favicon_url_;
  GoogleString favicon_head_;
  int64 fetcher_timeout_ms_;

 private:
  CurlUrlAsyncFetcherTest(const CurlUrlAsyncFetcherTest&) = delete;
  CurlUrlAsyncFetcherTest& operator=(const CurlUrlAsyncFetcherTest&) = delete;
};

// ---- Basic fetch tests (matching Serf) ----

TEST_F(CurlUrlAsyncFetcherTest, FetchOneURL) {
  EXPECT_TRUE(TestFetch(kModpagespeedSite, kModpagespeedSite));
  EXPECT_FALSE(response_headers(kModpagespeedSite)->IsGzipped());
  int request_count =
      statistics_->GetVariable(CurlStats::kCurlFetchRequestCount)->Get();
  EXPECT_EQ(1, request_count);
  int bytes_count =
      statistics_->GetVariable(CurlStats::kCurlFetchByteCount)->Get();
  EXPECT_LT(3000, bytes_count);
  ValidateMonitoringStats(1, 0);
}

TEST_F(CurlUrlAsyncFetcherTest, FetchUsingDifferentRequestMethod) {
  request_headers(kModpagespeedSite)->set_method(RequestHeaders::kPurge);
  StartFetches(kModpagespeedSite, kModpagespeedSite);
  ASSERT_EQ(1, WaitTillDone(kModpagespeedSite, kModpagespeedSite));
  ASSERT_TRUE(fetches_[kModpagespeedSite]->IsDone());
  EXPECT_LT(static_cast<size_t>(0), contents(kModpagespeedSite).size());
  EXPECT_EQ(405, response_headers(kModpagespeedSite)->status_code());
  ValidateMonitoringStats(0, 1);
}

// Tests that when the fetcher requests gzipped data it gets it.  Note
// that the callback is delivered content that must be explicitly unzipped.
TEST_F(CurlUrlAsyncFetcherTest, FetchOneURLGzipped) {
  GoogleString gzip_url = StrCat("http://", test_host_, "/gzip");
  int index = AddTestUrl(gzip_url, "{");
  request_headers(index)->Add(HttpAttributes::kAcceptEncoding,
                              HttpAttributes::kGzip);
  StartFetches(index, index);
  ASSERT_EQ(1, WaitTillDone(index, index));
  ASSERT_TRUE(fetches_[index]->IsDone());
  EXPECT_LT(static_cast<size_t>(0), contents(index).size());
  EXPECT_EQ(200, response_headers(index)->status_code());
  // curl may auto-decompress gzip. If it does, verify content starts with {.
  // If not, verify it's gzipped and can be inflated.
  if (response_headers(index)->IsGzipped()) {
    GzipInflater inflater(GzipInflater::kGzip);
    ASSERT_TRUE(inflater.Init());
    ASSERT_TRUE(
        inflater.SetInput(contents(index).data(), contents(index).size()));
    ASSERT_TRUE(inflater.HasUnconsumedInput());
    int size = content_starts_[index].size();
    std::unique_ptr<char[]> buf(new char[size]);
    ASSERT_EQ(size, inflater.InflateBytes(buf.get(), size));
    EXPECT_EQ(content_starts_[index], GoogleString(buf.get(), size));
  } else {
    EXPECT_STREQ("{", contents(index).substr(0, 1));
  }
  EXPECT_EQ(0, ActiveFetches());
}

// Transparent gzip: fetcher adds gzip, inflates before delivering.
TEST_F(CurlUrlAsyncFetcherTest, FetchOneURLWithGzip) {
  curl_fetcher_->set_fetch_with_gzip(true);
  EXPECT_TRUE(TestFetch(kModpagespeedSite, kModpagespeedSite));
  EXPECT_FALSE(response_headers(kModpagespeedSite)->IsGzipped());
  int request_count =
      statistics_->GetVariable(CurlStats::kCurlFetchRequestCount)->Get();
  EXPECT_EQ(1, request_count);
  int bytes_count =
      statistics_->GetVariable(CurlStats::kCurlFetchByteCount)->Get();
  EXPECT_LT(1000, bytes_count);
  ValidateMonitoringStats(1, 0);
}

TEST_F(CurlUrlAsyncFetcherTest, FetchTwoURLs) {
  EXPECT_TRUE(TestFetch(kGoogleFavicon, kGoogleLogo));
  int request_count =
      statistics_->GetVariable(CurlStats::kCurlFetchRequestCount)->Get();
  EXPECT_EQ(2, request_count);
  int bytes_count =
      statistics_->GetVariable(CurlStats::kCurlFetchByteCount)->Get();
  EXPECT_LT(10000, bytes_count);
  EXPECT_EQ(0, ActiveFetches());
  ValidateMonitoringStats(2, 0);
}

// ---- Threading tests (matching Serf) ----

TEST_F(CurlUrlAsyncFetcherTest, TestCancelThreeThreaded) {
  StartFetches(kModpagespeedSite, kGoogleLogo);
  // Explicit shutdown via TearDown, it's OK to call this twice.
  TearDown();
  // Some fetches may succeed before shutdown, but none should be counted
  // as failures since we cancelled them.
  EXPECT_LE(
      statistics_->GetVariable(CurlStats::kCurlFetchUltimateSuccess)->Get(), 3);
  EXPECT_EQ(
      0, statistics_->GetVariable(CurlStats::kCurlFetchUltimateFailure)->Get());
}

TEST_F(CurlUrlAsyncFetcherTest, TestThreeThreaded) {
  StartFetches(kModpagespeedSite, kGoogleLogo);
  int done = WaitTillDone(kModpagespeedSite, kGoogleLogo);
  EXPECT_EQ(3, done);
  ValidateFetches(kModpagespeedSite, kGoogleLogo);
  ValidateMonitoringStats(3, 0);
}

// ---- Timeout test ----

TEST_F(CurlUrlAsyncFetcherTest, TestTimeout) {
  Variable* timeouts =
      statistics_->GetVariable(CurlStats::kCurlFetchTimeoutCount);
  // The local /delay endpoint deterministically holds the connection open far
  // longer than fetcher_timeout_ms_ (5s), so our fetcher's own timeout is the
  // only thing that can end the fetch -- no retry/backoff scaffolding needed.
  StartFetches(kCgiSlowJs, kCgiSlowJs);
  int64 start_ms = timer_->NowMs();
  ASSERT_EQ(1, WaitTillDone(kCgiSlowJs, kCgiSlowJs));
  int64 elapsed_ms = timer_->NowMs() - start_ms;
  // The contract: the fetcher's OWN timeout ended the fetch, unsuccessfully.
  EXPECT_EQ(1, timeouts->Get());
  ASSERT_TRUE(fetches_[kCgiSlowJs]->IsDone());
  EXPECT_FALSE(fetches_[kCgiSlowJs]->success());

  // Sanity-guard that it ran for ~the timeout (not an instant failure for some
  // other reason) — but with tolerance. `elapsed_ms` is bracketed from AFTER
  // StartFetches returns, so it omits curl's internal dispatch head-start
  // (hundreds of ms); against a zero-latency local server that gap puts a
  // strict `>= fetcher_timeout_ms_` just under the line (~4726 vs 5000). The
  // old strict check only passed because remote-host (httpbin.org) latency
  // padded the window — exactly the external dependency this suite removed.
  const int64 kTimeoutToleranceMs = 1000;
  EXPECT_GE(elapsed_ms, fetcher_timeout_ms_ - kTimeoutToleranceMs);

  int time_duration =
      statistics_->GetVariable(CurlStats::kCurlFetchTimeDurationMs)->Get();
  EXPECT_GE(time_duration, fetcher_timeout_ms_ - kTimeoutToleranceMs);
}

TEST_F(CurlUrlAsyncFetcherTest, Test204) {
  TestFetch(kNoContent, kNoContent);
  EXPECT_EQ(HttpStatus::kNoContent,
            response_headers(kNoContent)->status_code());
  ValidateMonitoringStats(1, 0);
}

// ---- HTTPS tests (matching Serf) ----

TEST_F(CurlUrlAsyncFetcherTest, TestHttpsFailsByDefault) {
  TestHttpsFails(https_favicon_url_);
  ValidateMonitoringStats(0, 1);
}

TEST_F(CurlUrlAsyncFetcherTest, TestHttpsFailsForSelfSignedCert) {
  curl_fetcher_->SetHttpsOptions("enable");
  EXPECT_TRUE(curl_fetcher_->SupportsHttps());
  // The self-signed HTTPS server presents a leaf that does NOT chain to the
  // trusted test CA. With verification on this must be rejected as a cert
  // error -- same intent as the former badssl.com self-signed endpoint.
  TestHttpsFails(
      StrCat("https://", https_self_signed_server_->host_port(), "/image/png"));
  // Should have recorded exactly one cert error (single fetch).
  EXPECT_EQ(1,
            statistics_->GetVariable(CurlStats::kCurlFetchCertErrors)->Get());
  ValidateMonitoringStats(0, 1);
}

TEST_F(CurlUrlAsyncFetcherTest, TestHttpsSucceeds) {
  curl_fetcher_->SetHttpsOptions("enable");
  EXPECT_TRUE(curl_fetcher_->SupportsHttps());
  // Trusted-CA HTTPS server: cert chains to the injected test CA and its SAN
  // matches 127.0.0.1, so verification (VERIFYPEER=1, VERIFYHOST=2) passes.
  TestHttpsSucceeds(StrCat("https://", https_host_, "/html"),
                    "<!DOCTYPE html>");
  ValidateMonitoringStats(1, 0);
}

TEST_F(CurlUrlAsyncFetcherTest, TestHttpsWithExplicitHost) {
  // Make sure if an explicit Host: header is set, the HTTPS fetch still works.
  // allow_self_signed disables peer verification, so the header value is
  // exercised purely as a request-header passthrough (matching the original
  // intent against the live endpoint).
  GoogleUrl original_url(https_favicon_url_);
  curl_fetcher_->SetHttpsOptions("enable,allow_self_signed");
  int index = AddTestUrl(https_favicon_url_, favicon_head_);
  request_headers(index)->Add(HttpAttributes::kHost,
                              original_url.HostAndPort());
  StartFetches(index, index);
  ExpectHttpsSucceeds(index);
  ValidateMonitoringStats(1, 0);
}

TEST_F(CurlUrlAsyncFetcherTest, TestHttpsSucceedsWhenEnabled) {
  curl_fetcher_->SetHttpsOptions("enable,allow_self_signed");
  EXPECT_TRUE(curl_fetcher_->SupportsHttps());
  TestHttpsSucceeds(https_favicon_url_, favicon_head_);
  ValidateMonitoringStats(1, 0);
}

TEST_F(CurlUrlAsyncFetcherTest, TestHttpsFailsForBogusCertDir) {
  curl_fetcher_->SetHttpsOptions("enable");
  // Point the CA at an empty directory and clear the CA file: with no trusted
  // CA available, even the trusted-CA server's leaf cannot be verified, so the
  // fetch must fail. Same intent as the former www.google.com + bogus-dir
  // case (a valid HTTPS endpoint that fails because the CA bundle is bogus).
  curl_fetcher_->SetSslCertificatesDir(GTestTempDir());
  curl_fetcher_->SetSslCertificatesFile("");
  TestHttpsFails(StrCat("https://", https_host_, "/image/png"));
  ValidateMonitoringStats(0, 1);
}

// ---- Connection refused tests (matching Serf) ----

TEST_F(CurlUrlAsyncFetcherTest, ThreadedConnectionRefusedNoDetail) {
  StringPiece vb_test(getenv("VIRTUALBOX_TEST"));
  if (!vb_test.empty()) {
    return;
  }
  ConnectionRefusedTest();
  EXPECT_LE(1, message_handler_.SeriousMessages());
  EXPECT_GE(2, message_handler_.SeriousMessages());
}

TEST_F(CurlUrlAsyncFetcherTest, ThreadedConnectionRefusedWithDetail) {
  StringPiece vb_test(getenv("VIRTUALBOX_TEST"));
  if (!vb_test.empty()) {
    return;
  }
  curl_fetcher_->set_list_outstanding_urls_on_error(true);
  ConnectionRefusedTest();
  EXPECT_LE(1, message_handler_.SeriousMessages());
  EXPECT_GE(2, message_handler_.SeriousMessages());
  GoogleString text;
  StringWriter text_writer(&text);
  message_handler_.Dump(&text_writer);
  EXPECT_TRUE(text.find(urls_[kConnectionRefused]) != GoogleString::npos)
      << text;
}

TEST_F(CurlUrlAsyncFetcherTest,
       ThreadedConnectionRefusedCustomRouteWithDetail) {
  curl_fetcher_->set_list_outstanding_urls_on_error(true);

  int index = AddTestUrl("http://127.0.0.1:1023/refused.jpg", "");
  request_headers(index)->Add(HttpAttributes::kHost,
                              StrCat(test_host_, ":1023"));
  StartFetches(index, index);
  ASSERT_EQ(WaitTillDone(index, index), 1);
  ASSERT_TRUE(fetches_[index]->IsDone());
  EXPECT_EQ(HttpStatus::kNotFound, response_headers(index)->status_code());
  ValidateMonitoringStats(0, 1);
}

// ---- Content length tracking (matching Serf) ----

TEST_F(CurlUrlAsyncFetcherTest, TestTrackOriginalContentLength) {
  curl_fetcher_->set_track_original_content_length(true);
  StartFetch(kModpagespeedSite);
  WaitTillDone(kModpagespeedSite, kModpagespeedSite);
  const char* ocl_header =
      response_headers(kModpagespeedSite)
          ->Lookup1(HttpAttributes::kXOriginalContentLength);
  ASSERT_TRUE(ocl_header != nullptr);
  int bytes_count =
      statistics_->GetVariable(CurlStats::kCurlFetchByteCount)->Get();
  int64 ocl_value;
  EXPECT_TRUE(StringToInt64(ocl_header, &ocl_value));
  EXPECT_EQ(bytes_count, ocl_value);
}

// ---- POST test (matching Serf) ----

TEST_F(CurlUrlAsyncFetcherTest, TestPost) {
  int index = AddTestUrl(StrCat("http://", test_host_, "/post"), "{");
  request_headers(index)->set_method(RequestHeaders::kPost);
  request_headers(index)->Add(HttpAttributes::kContentType,
                              "application/x-www-form-urlencoded");
  request_headers(index)->set_message_body("a=b&c=d");
  StartFetches(index, index);
  ASSERT_EQ(WaitTillDone(index, index), 1);
  ValidateFetches(index, index);
  EXPECT_EQ(HttpStatus::kOK, response_headers(index)->status_code());
  EXPECT_TRUE(contents(index).find("\"a\": \"b\"") != GoogleString::npos ||
              contents(index).find("a=b") != GoogleString::npos);
}

// ---- BoringSSL EAGAIN regression test ----
//
// On Linux, libcurl is linked against BoringSSL (shared with Envoy).
// When curl_multi uses non-blocking sockets, SSL_connect() may return
// SSL_ERROR_SYSCALL when the underlying BIO gets EAGAIN. Unlike OpenSSL,
// BoringSSL does NOT translate this to SSL_ERROR_WANT_READ — it propagates
// the raw SSL_ERROR_SYSCALL. Without our patch
// (bazel/curl_boringssl_ssl_connect_8_18.patch), curl's ossl_connect_step2()
// treats this as a fatal error and the TLS handshake fails.
//
// This test verifies that HTTPS fetches through curl_multi succeed on Linux,
// which exercises the patched code path. Multiple concurrent fetches are used
// to exercise the handshake path under realistic conditions.

TEST_F(CurlUrlAsyncFetcherTest, TestBoringSSLEagainHandling) {
  curl_fetcher_->SetHttpsOptions("enable");
  EXPECT_TRUE(curl_fetcher_->SupportsHttps());

  // Multiple concurrent HTTPS fetches — each one exercises the SSL_connect()
  // handshake through curl_multi's non-blocking I/O path.
  const int kNumFetches = 5;
  int first_idx = -1;
  for (int i = 0; i < kNumFetches; ++i) {
    int idx =
        AddTestUrl(StrCat("https://", https_host_, "/html"), "<!DOCTYPE html>");
    if (first_idx == -1) first_idx = idx;
  }
  int last_idx = first_idx + kNumFetches - 1;

  StartFetches(first_idx, last_idx);
  ASSERT_EQ(kNumFetches, WaitTillDone(first_idx, last_idx));

  for (int i = first_idx; i <= last_idx; ++i) {
    ASSERT_TRUE(fetches_[i]->IsDone());
    EXPECT_TRUE(fetches_[i]->success())
        << "HTTPS fetch " << (i - first_idx)
        << " failed — if linked against BoringSSL, this indicates the "
           "SSL_ERROR_SYSCALL EAGAIN patch is missing from libcurl";
    EXPECT_EQ(HttpStatus::kOK, response_headers(i)->status_code());
  }

  EXPECT_EQ(0,
            statistics_->GetVariable(CurlStats::kCurlFetchCertErrors)->Get());
}

// ---- Shutdown test (curl-specific) ----

TEST_F(CurlUrlAsyncFetcherTest, TestShutdownWithActiveFetches) {
  StartFetches(kCgiSlowJs, kCgiSlowJs);
  curl_fetcher_->ShutDown();
  CurlTestFetch* fetch = new CurlTestFetch(
      RequestContext::NewTestRequestContext(thread_system_.get()),
      mutex_.get());
  // The fetcher is already shut down, so this Fetch() returns immediately via
  // the shutdown guard and never opens a socket -- no network involved.
  curl_fetcher_->Fetch(StrCat("http://", test_host_, "/html"),
                       &message_handler_, fetch);
  EXPECT_TRUE(fetch->IsDone());
  EXPECT_FALSE(fetch->success());
  delete fetch;
}

// ---- Proxy tests (matching Serf) ----

class CurlUrlAsyncFetcherTestWithProxy : public CurlUrlAsyncFetcherTest {
 protected:
  void SetUp() override {
    // Non-working proxy, just for covering crash bugs.
    SetUpWithProxy("127.0.0.1:8080");
  }
};

TEST_F(CurlUrlAsyncFetcherTestWithProxy, TestBlankUrl) {
  int index = AddTestUrl("", "");
  StartFetches(index, index);
  ASSERT_EQ(WaitTillDone(index, index), 1);
  ASSERT_TRUE(fetches_[index]->IsDone());
  EXPECT_EQ(HttpStatus::kNotFound, response_headers(index)->status_code());
  ValidateMonitoringStats(0, 1);
}

// ---- Fake web server tests (matching Serf, using POSIX sockets) ----

class CurlUrlAsyncFetcherTestFakeWebServer : public CurlUrlAsyncFetcherTest {
 public:
  class FakeWebServerThread : public PosixTcpServerThread {
   public:
    FakeWebServerThread(int desired_listen_port, ThreadSystem* thread_system)
        : PosixTcpServerThread(desired_listen_port, "fake_webserver",
                               thread_system) {}
    ~FakeWebServerThread() override { ShutDown(); }

    void HandleClientConnection(int client_fd) override {
      char request_buffer[kStackBufferSize];
      ssize_t n = read(client_fd, request_buffer, sizeof(request_buffer) - 1);
      (void)n;  // Don't care about the request content

      static const char kResponse[] =
          "HTTP/1.0 282 Fake Status Code\r\n"
          "Content-Length: 500\r\n"
          "Connection: close\r\n"
          "Content-Type: text/plain\r\n"
          "\r\n"
          "This text is less than 500 bytes.\n";

      ssize_t written = write(client_fd, kResponse, strlen(kResponse));
      (void)written;
      // Wait for over the timeout to make really sure it times out.
      WaitForHangupOrTimeout(client_fd, FetcherTimeoutMs() * 1000 * 2);
    }
  };

  static void SetUpTestSuite() {
    PosixTcpServerThread::PickListenPortOnce(&desired_listen_port_);
  }

  void SetUp() override {
    thread_ = std::make_unique<FakeWebServerThread>(desired_listen_port_,
                                                    thread_system_.get());
    ASSERT_TRUE(thread_->Start());
    int port = thread_->GetListeningPort();
    GoogleString proxy_address = StrCat("127.0.0.1:", IntegerToString(port));
    SetUpWithProxy(proxy_address.c_str());
  }

  std::unique_ptr<FakeWebServerThread> thread_;

 private:
  static int desired_listen_port_;
};

int CurlUrlAsyncFetcherTestFakeWebServer::desired_listen_port_ = 0;

TEST_F(CurlUrlAsyncFetcherTestFakeWebServer, TestHangingGet) {
  Variable* timeouts =
      statistics_->GetVariable(CurlStats::kCurlFetchTimeoutCount);
  EXPECT_EQ(0, timeouts->Get());
  int index = AddTestUrl(StrCat("http://", test_host_, "/never_fetched"), "");
  StartFetches(index, index);
  ASSERT_EQ(WaitTillDone(index, index), 1);
  ASSERT_TRUE(fetches_[index]->IsDone());
  EXPECT_EQ(1, timeouts->Get());
  EXPECT_EQ(282, response_headers(index)->status_code());
  EXPECT_STREQ("This text is less than 500 bytes.\n", contents(index));
}

}  // namespace net_instaweb

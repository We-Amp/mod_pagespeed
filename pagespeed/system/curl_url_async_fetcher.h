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

#ifndef PAGESPEED_SYSTEM_CURL_URL_ASYNC_FETCHER_H_
#define PAGESPEED_SYSTEM_CURL_URL_ASYNC_FETCHER_H_

#include <curl/curl.h>

#include <atomic>
#include <memory>
#include <vector>

#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/pool.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/system/curl_fetch.h"

namespace net_instaweb {

class AbstractMutex;
class AsyncFetch;
class MessageHandler;
class ResponseHeaders;
class Statistics;
class Timer;
class Variable;
class UpDownCounter;

struct CurlStats {
  static const char kCurlFetchRequestCount[];
  static const char kCurlFetchByteCount[];
  static const char kCurlFetchTimeDurationMs[];
  static const char kCurlFetchCancelCount[];
  static const char kCurlFetchActiveCount[];
  static const char kCurlFetchTimeoutCount[];
  static const char kCurlFetchFailureCount[];
  static const char kCurlFetchCertErrors[];
  static const char kCurlFetchReadCalls[];
  static const char kCurlFetchUltimateSuccess[];
  static const char kCurlFetchUltimateFailure[];
  static const char kCurlFetchLastCheckTimestampMs[];
};

class CurlUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  CurlUrlAsyncFetcher(const char* proxy, ThreadSystem* thread_system,
                      Statistics* statistics, Timer* timer, int64 timeout_ms,
                      MessageHandler* handler);

  ~CurlUrlAsyncFetcher() override;

  static void InitStats(Statistics* statistics);

  void ShutDown() override;

  bool SupportsHttps() const override { return https_options_ != 0; }

  void Fetch(const GoogleString& url, MessageHandler* message_handler,
             AsyncFetch* callback) override;

  // SSL/TLS Configuration
  void SetSslCertificatesDir(StringPiece dir);
  const GoogleString& ssl_certificates_dir() const {
    return ssl_certificates_dir_;
  }

  void SetSslCertificatesFile(StringPiece file);
  const GoogleString& ssl_certificates_file() const {
    return ssl_certificates_file_;
  }

  bool SetHttpsOptions(StringPiece directive);
  static bool ValidateHttpsOptions(StringPiece directive,
                                   GoogleString* error_message);
  // Parses a FetchHttps directive into HttpsOptions flags. Public so other
  // fetchers (the nginx native fetcher) share the same interpretation.
  static bool ParseHttpsOptions(StringPiece directive, uint32* options,
                                GoogleString* error_message);

  enum HttpsOptions {
    kEnableHttps = 1 << 0,
    kAllowSelfSigned = 1 << 1,
    kAllowUnknownCertificateAuthority = 1 << 2,
    kAllowCertificateNotYetValid = 1 << 3,
  };

  bool allow_https() const { return (https_options_ & kEnableHttps) != 0; }
  bool allow_self_signed() const {
    return (https_options_ & kAllowSelfSigned) != 0;
  }
  bool allow_unknown_certificate_authority() const {
    return (https_options_ & kAllowUnknownCertificateAuthority) != 0;
  }
  bool allow_certificate_not_yet_valid() const {
    return (https_options_ & kAllowCertificateNotYetValid) != 0;
  }

  void FetchComplete(CurlFetch* fetch);
  void PrintActiveFetches(MessageHandler* handler) const;

  void ReportCompletedFetchStats(const CurlFetch* fetch);
  void ReportFetchSuccessStats(CurlFetchCompletionResult result,
                               const ResponseHeaders* headers,
                               const CurlFetch* fetch);
  void IncrementCertErrors();

  bool track_original_content_length() {
    return track_original_content_length_;
  }
  void set_track_original_content_length(bool x) {
    track_original_content_length_ = x;
  }

  int64 timeout_ms() override { return fetch_timeout_; }

  ThreadSystem* thread_system() { return thread_system_; }

  void set_list_outstanding_urls_on_error(bool x) {
    list_outstanding_urls_on_error_ = x;
  }
  bool list_outstanding_urls_on_error() const {
    return list_outstanding_urls_on_error_;
  }

  virtual bool AnyPendingFetches() {
    return !active_fetches_.empty() || !pending_fetches_.empty();
  }

  int ApproximateNumActiveFetches() {
    return active_fetches_.size() + pending_fetches_.size();
  }

  void CancelActiveFetches();

  bool shutdown() const { return shutdown_; }

  const GoogleString& proxy() const { return proxy_; }

  // Called by the poll thread. Public for access from the thread class.
  void PollLoop();

 protected:
  using CurlFetchPool = Pool<CurlFetch>;

 private:
  friend class CurlFetch;

  CURLM* multi_handle_;
  CurlFetchPool active_fetches_;
  CurlFetchPool pending_fetches_;
  CurlFetchPool completed_fetches_;
  GoogleString proxy_;

  std::atomic<bool> shutdown_;
  bool track_original_content_length_;
  bool list_outstanding_urls_on_error_;
  ThreadSystem* thread_system_;
  MessageHandler* message_handler_;
  std::unique_ptr<AbstractMutex> mutex_;
  std::unique_ptr<ThreadSystem::Thread> poll_thread_;

  Statistics* statistics_;
  Timer* timer_;
  int64 fetch_timeout_;

  // SSL/TLS configuration
  GoogleString ssl_certificates_dir_;
  GoogleString ssl_certificates_file_;
  uint32 https_options_;

  // Statistics variables
  Variable* request_count_;
  Variable* byte_count_stat_;
  Variable* time_duration_ms_;
  Variable* cancel_count_;
  UpDownCounter* active_count_;
  Variable* timeout_count_;
  Variable* failure_count_;
  Variable* cert_errors_;
  Variable* read_calls_count_;
  Variable* ultimate_success_;
  Variable* ultimate_failure_;
  UpDownCounter* last_check_timestamp_ms_;

  CurlUrlAsyncFetcher(const CurlUrlAsyncFetcher&) = delete;
  CurlUrlAsyncFetcher& operator=(const CurlUrlAsyncFetcher&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_CURL_URL_ASYNC_FETCHER_H_

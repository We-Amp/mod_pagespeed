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

#include <algorithm>
#include <cstring>
#include <vector>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/inflating_fetch.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/pool.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

const char CurlStats::kCurlFetchRequestCount[] = "curl_fetch_request_count";
const char CurlStats::kCurlFetchByteCount[] = "curl_fetch_bytes_count";
const char CurlStats::kCurlFetchTimeDurationMs[] =
    "curl_fetch_time_duration_ms";
const char CurlStats::kCurlFetchCancelCount[] = "curl_fetch_cancel_count";
const char CurlStats::kCurlFetchActiveCount[] = "curl_fetch_active_count";
const char CurlStats::kCurlFetchTimeoutCount[] = "curl_fetch_timeout_count";
const char CurlStats::kCurlFetchFailureCount[] = "curl_fetch_failure_count";
const char CurlStats::kCurlFetchCertErrors[] = "curl_fetch_cert_errors";
const char CurlStats::kCurlFetchReadCalls[] = "curl_fetch_num_calls_to_read";
const char CurlStats::kCurlFetchUltimateSuccess[] =
    "curl_fetch_ultimate_success";
const char CurlStats::kCurlFetchUltimateFailure[] =
    "curl_fetch_ultimate_failure";
const char CurlStats::kCurlFetchLastCheckTimestampMs[] =
    "curl_fetch_last_check_timestamp_ms";

namespace {

class CurlPollThread : public ThreadSystem::Thread {
 public:
  CurlPollThread(CurlUrlAsyncFetcher* fetcher, ThreadSystem* thread_system)
      : Thread(thread_system, "curl_poll", ThreadSystem::kJoinable),
        fetcher_(fetcher) {}

  void Run() override { fetcher_->PollLoop(); }

 private:
  CurlUrlAsyncFetcher* fetcher_;
};

}  // namespace

CurlUrlAsyncFetcher::CurlUrlAsyncFetcher(const char* proxy,
                                         ThreadSystem* thread_system,
                                         Statistics* statistics, Timer* timer,
                                         int64 timeout_ms,
                                         MessageHandler* handler)
    : multi_handle_(nullptr),
      shutdown_(false),
      track_original_content_length_(false),
      list_outstanding_urls_on_error_(false),
      thread_system_(thread_system),
      message_handler_(handler),
      mutex_(thread_system->NewMutex()),
      statistics_(statistics),
      timer_(timer),
      fetch_timeout_(timeout_ms),
      https_options_(0),
      request_count_(nullptr),
      byte_count_stat_(nullptr),
      time_duration_ms_(nullptr),
      cancel_count_(nullptr),
      active_count_(nullptr),
      timeout_count_(nullptr),
      failure_count_(nullptr),
      cert_errors_(nullptr),
      read_calls_count_(nullptr),
      ultimate_success_(nullptr),
      ultimate_failure_(nullptr),
      last_check_timestamp_ms_(nullptr) {
  if (proxy != nullptr && proxy[0] != '\0') {
    proxy_ = proxy;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  multi_handle_ = curl_multi_init();

  if (statistics != nullptr) {
    request_count_ = statistics->GetVariable(CurlStats::kCurlFetchRequestCount);
    byte_count_stat_ = statistics->GetVariable(CurlStats::kCurlFetchByteCount);
    time_duration_ms_ =
        statistics->GetVariable(CurlStats::kCurlFetchTimeDurationMs);
    cancel_count_ = statistics->GetVariable(CurlStats::kCurlFetchCancelCount);
    active_count_ =
        statistics->GetUpDownCounter(CurlStats::kCurlFetchActiveCount);
    timeout_count_ = statistics->GetVariable(CurlStats::kCurlFetchTimeoutCount);
    failure_count_ = statistics->GetVariable(CurlStats::kCurlFetchFailureCount);
    cert_errors_ = statistics->GetVariable(CurlStats::kCurlFetchCertErrors);
#ifndef NDEBUG
    read_calls_count_ = statistics->GetVariable(CurlStats::kCurlFetchReadCalls);
#endif
    ultimate_success_ =
        statistics->GetVariable(CurlStats::kCurlFetchUltimateSuccess);
    ultimate_failure_ =
        statistics->GetVariable(CurlStats::kCurlFetchUltimateFailure);
    last_check_timestamp_ms_ =
        statistics->GetUpDownCounter(CurlStats::kCurlFetchLastCheckTimestampMs);
  }

  // Start the poll thread
  poll_thread_ = std::make_unique<CurlPollThread>(this, thread_system);
  poll_thread_->Start();
}

CurlUrlAsyncFetcher::~CurlUrlAsyncFetcher() {
  if (!shutdown_) {
    ShutDown();
  }
  if (multi_handle_ != nullptr) {
    curl_multi_cleanup(multi_handle_);
  }
  curl_global_cleanup();
}

void CurlUrlAsyncFetcher::InitStats(Statistics* statistics) {
  statistics->AddVariable(CurlStats::kCurlFetchRequestCount);
  statistics->AddVariable(CurlStats::kCurlFetchByteCount);
  statistics->AddVariable(CurlStats::kCurlFetchTimeDurationMs);
  statistics->AddVariable(CurlStats::kCurlFetchCancelCount);
  statistics->AddUpDownCounter(CurlStats::kCurlFetchActiveCount);
  statistics->AddVariable(CurlStats::kCurlFetchTimeoutCount);
  statistics->AddVariable(CurlStats::kCurlFetchFailureCount);
  statistics->AddVariable(CurlStats::kCurlFetchCertErrors);
#ifndef NDEBUG
  statistics->AddVariable(CurlStats::kCurlFetchReadCalls);
#endif
  statistics->AddVariable(CurlStats::kCurlFetchUltimateSuccess);
  statistics->AddVariable(CurlStats::kCurlFetchUltimateFailure);
  statistics->AddUpDownCounter(CurlStats::kCurlFetchLastCheckTimestampMs);
}

void CurlUrlAsyncFetcher::ShutDown() {
  shutdown_ = true;
  if (poll_thread_ != nullptr) {
    poll_thread_->Join();
    poll_thread_.reset();
  }
}

void CurlUrlAsyncFetcher::Fetch(const GoogleString& url,
                                MessageHandler* message_handler,
                                AsyncFetch* async_fetch) {
  if (shutdown_) {
    message_handler->Message(
        kWarning, "CurlUrlAsyncFetcher is shut down, cannot fetch: %s",
        url.c_str());
    async_fetch->Done(false);
    return;
  }

  // Reject HTTPS URLs when HTTPS is not enabled
  if (!allow_https() && StringCaseStartsWith(url, "https:")) {
    message_handler->Message(
        kWarning, "HTTPS fetching is disabled, cannot fetch: %s", url.c_str());
    async_fetch->response_headers()->set_status_code(HttpStatus::kNotFound);
    async_fetch->HeadersComplete();
    async_fetch->Done(false);
    if (failure_count_ != nullptr) {
      failure_count_->Add(1);
    }
    if (ultimate_failure_ != nullptr) {
      ultimate_failure_->Add(1);
    }
    return;
  }

  async_fetch = EnableInflation(async_fetch);

  CurlFetch* curl_fetch =
      new CurlFetch(url, async_fetch, message_handler, timer_);

  if (!curl_fetch->InitCurl(this)) {
    message_handler->Message(kError, "Failed to initialize curl fetch: %s",
                             url.c_str());
    delete curl_fetch;
    async_fetch->Done(false);
    return;
  }

  if (active_count_ != nullptr) {
    active_count_->Add(1);
  }

  {
    ScopedMutex lock(mutex_.get());
    pending_fetches_.Add(curl_fetch);
  }
}

void CurlUrlAsyncFetcher::PollLoop() {
  while (!shutdown_) {
    // Move pending fetches to active
    {
      ScopedMutex lock(mutex_.get());
      while (!pending_fetches_.empty()) {
        CurlFetch* fetch = pending_fetches_.RemoveOldest();
        active_fetches_.Add(fetch);
        curl_multi_add_handle(multi_handle_, fetch->curl_handle());
      }
    }
    // Drive transfers
    int still_running = 0;
    curl_multi_perform(multi_handle_, &still_running);

    // Process completions
    int msgs_in_queue;
    CURLMsg* msg;
    while ((msg = curl_multi_info_read(multi_handle_, &msgs_in_queue)) !=
           nullptr) {
      if (msg->msg == CURLMSG_DONE) {
        CURL* easy = msg->easy_handle;
        CurlFetch* fetch = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &fetch);
        curl_multi_remove_handle(multi_handle_, easy);

        {
          ScopedMutex lock(mutex_.get());
          active_fetches_.Remove(fetch);
        }

        fetch->Done(msg->data.result);
      }
    }

    // Wait for activity or 10ms timeout
    int numfds;
    curl_multi_poll(multi_handle_, nullptr, 0, 10, &numfds);
  }

  // Drain pending fetches (under lock) then cancel them (without lock)
  std::vector<CurlFetch*> to_cancel;
  {
    ScopedMutex lock(mutex_.get());
    while (!pending_fetches_.empty()) {
      to_cancel.push_back(pending_fetches_.RemoveOldest());
    }
  }
  for (CurlFetch* fetch : to_cancel) {
    fetch->Done(CURLE_ABORTED_BY_CALLBACK);
  }

  // Cancel active fetches (only poll thread touches active_fetches_ here)
  while (!active_fetches_.empty()) {
    CurlFetch* fetch = active_fetches_.RemoveOldest();
    curl_multi_remove_handle(multi_handle_, fetch->curl_handle());
    fetch->Done(CURLE_ABORTED_BY_CALLBACK);
  }
}

void CurlUrlAsyncFetcher::FetchComplete(CurlFetch* fetch) {
  ScopedMutex lock(mutex_.get());
  completed_fetches_.Add(fetch);
}

void CurlUrlAsyncFetcher::PrintActiveFetches(MessageHandler* handler) const {
  ScopedMutex lock(mutex_.get());
  if (!list_outstanding_urls_on_error_ && active_fetches_.empty()) {
    return;
  }
  handler->Message(
      kInfo, "Active fetches (%d):", static_cast<int>(active_fetches_.size()));
  for (auto it = active_fetches_.begin(); it != active_fetches_.end(); ++it) {
    CurlFetch* fetch = *it;
    handler->Message(kInfo, "  %s (started %ldms ago)", fetch->url().c_str(),
                     static_cast<long>(fetch->TimeDuration()));
  }
}

void CurlUrlAsyncFetcher::CancelActiveFetches() {
  ScopedMutex lock(mutex_.get());
  if (cancel_count_ != nullptr) {
    cancel_count_->Add(active_fetches_.size());
  }
  while (!active_fetches_.empty()) {
    CurlFetch* fetch = active_fetches_.oldest();
    active_fetches_.Remove(fetch);
    completed_fetches_.Add(fetch);
  }
}

void CurlUrlAsyncFetcher::SetSslCertificatesDir(StringPiece dir) {
  dir.CopyToString(&ssl_certificates_dir_);
}

void CurlUrlAsyncFetcher::SetSslCertificatesFile(StringPiece file) {
  file.CopyToString(&ssl_certificates_file_);
}

bool CurlUrlAsyncFetcher::ParseHttpsOptions(StringPiece directive,
                                            uint32* options,
                                            GoogleString* error_message) {
  *options = 0;
  StringPieceVector keywords;
  SplitStringPieceToVector(directive, ",", &keywords, true);

  for (const StringPiece& keyword : keywords) {
    StringPiece trimmed = keyword;
    TrimWhitespace(&trimmed);

    if (StringCaseEqual(trimmed, "enable")) {
      *options |= kEnableHttps;
    } else if (StringCaseEqual(trimmed, "disable")) {
      *options &= ~kEnableHttps;
    } else if (StringCaseEqual(trimmed, "allow_self_signed")) {
      *options |= kAllowSelfSigned;
    } else if (StringCaseEqual(trimmed,
                               "allow_unknown_certificate_authority")) {
      *options |= kAllowUnknownCertificateAuthority;
    } else if (StringCaseEqual(trimmed, "allow_certificate_not_yet_valid")) {
      *options |= kAllowCertificateNotYetValid;
    } else {
      *error_message =
          StrCat("Invalid HTTPS option keyword: ", trimmed.as_string());
      return false;
    }
  }
  return true;
}

bool CurlUrlAsyncFetcher::SetHttpsOptions(StringPiece directive) {
  GoogleString error_message;
  uint32 options;
  if (!ParseHttpsOptions(directive, &options, &error_message)) {
    message_handler_->Message(kError, "%s", error_message.c_str());
    return false;
  }
  https_options_ = options;
  return true;
}

bool CurlUrlAsyncFetcher::ValidateHttpsOptions(StringPiece directive,
                                               GoogleString* error_message) {
  uint32 options;
  return ParseHttpsOptions(directive, &options, error_message);
}

void CurlUrlAsyncFetcher::IncrementCertErrors() {
  if (cert_errors_ != nullptr) {
    cert_errors_->Add(1);
  }
}

void CurlUrlAsyncFetcher::ReportCompletedFetchStats(const CurlFetch* fetch) {
  if (request_count_ != nullptr) {
    request_count_->Add(1);
  }
  if (byte_count_stat_ != nullptr) {
    byte_count_stat_->Add(fetch->bytes_received());
  }
  if (time_duration_ms_ != nullptr) {
    time_duration_ms_->Add(fetch->TimeDuration());
  }
  if (active_count_ != nullptr) {
    active_count_->Add(-1);
  }
}

void CurlUrlAsyncFetcher::ReportFetchSuccessStats(
    CurlFetchCompletionResult result, const ResponseHeaders* headers,
    const CurlFetch* fetch) {
  if (result == CurlFetchCompletionResult::kClientCancel) {
    return;
  }
  if (result == CurlFetchCompletionResult::kSuccess) {
    if (headers != nullptr) {
      int status_code = headers->status_code();
      if (status_code >= 200 && status_code < 400) {
        if (ultimate_success_ != nullptr) {
          ultimate_success_->Add(1);
        }
      } else {
        if (ultimate_failure_ != nullptr) {
          ultimate_failure_->Add(1);
        }
      }
    }
  } else {
    if (result == CurlFetchCompletionResult::kTimeout) {
      if (timeout_count_ != nullptr) {
        timeout_count_->Add(1);
      }
    }
    if (failure_count_ != nullptr) {
      failure_count_->Add(1);
    }
    if (ultimate_failure_ != nullptr) {
      ultimate_failure_->Add(1);
    }
  }
}

}  // namespace net_instaweb

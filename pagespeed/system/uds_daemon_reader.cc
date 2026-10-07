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

#include "pagespeed/system/uds_daemon_reader.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/curl_url_async_fetcher.h"

namespace net_instaweb {

namespace {

// Wraps the caller's fetch for one read: counts body bytes, drops every
// byte past the caller's cap (an over-cap body is a failure, never a
// prefix), and records why the read failed before completing the caller's
// fetch.  Self-deletes on completion.
class CapEnforcingFetch : public SharedAsyncFetch {
 public:
  CapEnforcingFetch(AsyncFetch* base_fetch, size_t max_body_bytes, Timer* timer,
                    DaemonReadFailure* failure)
      : SharedAsyncFetch(base_fetch),
        max_body_bytes_(max_body_bytes),
        timer_(timer),
        start_ms_(timer->NowMs()),
        failure_(failure) {}

  // Route mapped writes through the counting HandleWrite as well.
  bool WriteMapped(const StringPiece& content,
                   const MappedSharedString& keepalive,
                   MessageHandler* handler) override {
    return Write(content, handler);
  }

 protected:
  bool HandleWrite(const StringPiece& content,
                   MessageHandler* handler) override {
    received_ += content.size();
    if (received_ > max_body_bytes_) {
      over_cap_ = true;
      return true;  // dropped
    }
    return SharedAsyncFetch::HandleWrite(content, handler);
  }

  bool HandleWriteShared(const StringPiece& content,
                         const SharedString& storage,
                         MessageHandler* handler) override {
    return HandleWrite(content, handler);
  }

  void HandleDone(bool success) override {
    if (over_cap_) {
      *failure_ = DaemonReadFailure::kTooLarge;
    } else if (!success) {
      const int64 elapsed_ms = timer_->NowMs() - start_ms_;
      *failure_ = elapsed_ms + UdsDaemonReader::kTimeoutClassificationSlackMs >=
                          UdsDaemonReader::kUpstreamTimeoutMs
                      ? DaemonReadFailure::kTimeout
                      : DaemonReadFailure::kDisconnected;
    } else {
      *failure_ = DaemonReadFailure::kNone;
    }
    SharedAsyncFetch::HandleDone(success && !over_cap_);
    delete this;
  }

 private:
  const size_t max_body_bytes_;
  Timer* timer_;
  const int64 start_ms_;
  DaemonReadFailure* failure_;
  size_t received_ = 0;
  bool over_cap_ = false;
};

}  // namespace

UdsDaemonReader::UdsDaemonReader(ThreadSystem* thread_system, Timer* timer,
                                 const GoogleString& socket_path,
                                 MessageHandler* message_handler)
    : thread_system_(thread_system),
      timer_(timer),
      socket_path_(socket_path),
      message_handler_(message_handler),
      mutex_(thread_system->NewMutex()) {}

UdsDaemonReader::~UdsDaemonReader() {
  // In-flight reads are normally long gone by now: the owning AdminSite
  // destroys the AdminDaemonHandler (which drains in-flight proxy fetches)
  // BEFORE this reader, so ShutDown() has nothing left to cancel.  If that
  // drain ever did time out, the fetcher's poll thread completes any
  // remaining reads with failure before ShutDown() joins it.
  if (fetcher_ != nullptr) {
    fetcher_->ShutDown();
  }
}

void UdsDaemonReader::Get(StringPiece daemon_path, size_t max_response_bytes,
                          AsyncFetch* fetch, DaemonReadFailure* failure) {
  CurlUrlAsyncFetcher* fetcher = GetOrCreateFetcher();
  if (fetcher == nullptr) {
    // Reader disabled (empty socket path).
    if (!fetch->headers_complete()) {
      fetch->response_headers()->set_status_code(HttpStatus::kBadGateway);
      fetch->HeadersComplete();
    }
    *failure = DaemonReadFailure::kDisabled;
    fetch->Done(false);
    return;
  }
  // The URL's host is ignored for routing over a unix socket; the path is
  // the caller's allow-listed daemon API path.  The caller's cap is
  // enforced by the wrapper; the transport's own cap sits above it.
  fetcher->FetchOverUnixSocket(
      StrCat("http://localhost", daemon_path), socket_path_, kUpstreamTimeoutMs,
      max_response_bytes + kTransportCapSlackBytes, message_handler_,
      new CapEnforcingFetch(fetch, max_response_bytes, timer_, failure));
}

CurlUrlAsyncFetcher* UdsDaemonReader::GetOrCreateFetcher() {
  if (socket_path_.empty()) {
    return nullptr;
  }
  ScopedMutex lock(mutex_.get());
  if (fetcher_ == nullptr) {
    // No proxy: unix-socket connections never leave the host.  No statistics:
    // these are admin-console reads, not origin fetches.
    fetcher_ = std::make_unique<CurlUrlAsyncFetcher>(
        "" /* proxy */, thread_system_, nullptr /* statistics */, timer_,
        kUpstreamTimeoutMs, message_handler_);
  }
  return fetcher_.get();
}

}  // namespace net_instaweb

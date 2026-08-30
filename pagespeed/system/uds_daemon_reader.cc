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
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/curl_url_async_fetcher.h"

namespace net_instaweb {

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

void UdsDaemonReader::Get(StringPiece daemon_path, AsyncFetch* fetch) {
  CurlUrlAsyncFetcher* fetcher = GetOrCreateFetcher();
  if (fetcher == nullptr) {
    // Reader disabled (empty socket path).
    if (!fetch->headers_complete()) {
      fetch->response_headers()->set_status_code(HttpStatus::kBadGateway);
      fetch->HeadersComplete();
    }
    fetch->Done(false);
    return;
  }
  // The URL's host is ignored for routing over a unix socket; the path is
  // the caller's allow-listed daemon API path.
  fetcher->FetchOverUnixSocket(StrCat("http://localhost", daemon_path),
                               socket_path_, kUpstreamTimeoutMs,
                               kMaxResponseBodyBytes, message_handler_, fetch);
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

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

#ifndef PAGESPEED_SYSTEM_CURL_FETCH_H_
#define PAGESPEED_SYSTEM_CURL_FETCH_H_

#include <curl/curl.h>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/pool.h"
#include "pagespeed/kernel/base/pool_element.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class AsyncFetch;
class CurlUrlAsyncFetcher;
class MessageHandler;
class Timer;

enum class CurlFetchCompletionResult {
  kSuccess,
  kFailure,
  kTimeout,
  kClientCancel,
};

class CurlFetch : public PoolElement<CurlFetch> {
 public:
  CurlFetch(const GoogleString& url, AsyncFetch* async_fetch,
            MessageHandler* message_handler, Timer* timer);
  ~CurlFetch();

  // Initialize the CURL easy handle with all options. Must be called
  // before adding to a CURLM multi handle. Returns true on success.
  bool InitCurl(CurlUrlAsyncFetcher* fetcher);

  // Connect over the unix-domain socket at `path` instead of TCP; the URL's
  // host is then ignored for routing.  Must be called before InitCurl.
  void set_unix_socket_path(StringPiece path) {
    path.CopyToString(&unix_socket_path_);
  }

  // Per-fetch overrides (must be called before InitCurl); the defaults
  // (0) keep the fetcher's configured timeout and the compile-time
  // response-body cap.
  void set_timeout_ms(int64 timeout_ms) { timeout_ms_ = timeout_ms; }
  void set_max_response_body_bytes(size_t max) {
    max_response_body_bytes_ = max;
  }

  // Called when curl reports this transfer is done.
  void Done(CURLcode result);

  CURL* curl_handle() const { return curl_handle_; }
  const GoogleString& url() const { return url_; }
  size_t bytes_received() const { return bytes_received_; }
  int64 fetch_start_ms() const { return fetch_start_ms_; }
  int64 TimeDuration() const;

 private:
  static size_t HeaderCallback(char* buffer, size_t size, size_t nmemb,
                               void* userdata);
  static size_t WriteCallback(char* buffer, size_t size, size_t nmemb,
                              void* userdata);

  void FixUserAgent();

  const GoogleString url_;
  CurlUrlAsyncFetcher* fetcher_;
  AsyncFetch* async_fetch_;
  MessageHandler* message_handler_;
  Timer* timer_;
  CURL* curl_handle_;
  struct curl_slist* request_headers_list_;
  bool headers_complete_;
  size_t bytes_received_;
  size_t header_bytes_received_;
  int64 fetch_start_ms_;
  int64 fetch_end_ms_;
  GoogleString unix_socket_path_;
  int64 timeout_ms_ = 0;                // 0: use the fetcher's timeout
  size_t max_response_body_bytes_ = 0;  // 0: use the compile-time cap

  CurlFetch(const CurlFetch&) = delete;
  CurlFetch& operator=(const CurlFetch&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_CURL_FETCH_H_

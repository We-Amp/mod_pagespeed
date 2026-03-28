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

#include "pagespeed/system/curl_fetch.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/public/version.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/curl_url_async_fetcher.h"

namespace net_instaweb {

// Maximum sizes to prevent resource exhaustion from malicious origins.
// Response body: 256 MB — covers all realistic web resources.
// Header total: 1 MB — far beyond any legitimate response header set.
static constexpr size_t kMaxResponseBodyBytes = 256 * 1024 * 1024;
static constexpr size_t kMaxHeaderBytes = 1 * 1024 * 1024;

CurlFetch::CurlFetch(const GoogleString& url, AsyncFetch* async_fetch,
                     MessageHandler* message_handler, Timer* timer)
    : url_(url),
      fetcher_(nullptr),
      async_fetch_(async_fetch),
      message_handler_(message_handler),
      timer_(timer),
      curl_handle_(nullptr),
      request_headers_list_(nullptr),
      headers_complete_(false),
      bytes_received_(0),
      header_bytes_received_(0),
      fetch_start_ms_(timer->NowMs()),
      fetch_end_ms_(0) {}

CurlFetch::~CurlFetch() {
  if (request_headers_list_ != nullptr) {
    curl_slist_free_all(request_headers_list_);
  }
  if (curl_handle_ != nullptr) {
    curl_easy_cleanup(curl_handle_);
  }
}

bool CurlFetch::InitCurl(CurlUrlAsyncFetcher* fetcher) {
  fetcher_ = fetcher;
  curl_handle_ = curl_easy_init();
  if (curl_handle_ == nullptr) {
    message_handler_->Message(kError, "Failed to create CURL easy handle");
    return false;
  }

  curl_easy_setopt(curl_handle_, CURLOPT_URL, url_.c_str());
  curl_easy_setopt(curl_handle_, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl_handle_, CURLOPT_WRITEDATA, this);
  curl_easy_setopt(curl_handle_, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl_handle_, CURLOPT_HEADERDATA, this);
  curl_easy_setopt(curl_handle_, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl_handle_, CURLOPT_PRIVATE, this);
  curl_easy_setopt(curl_handle_, CURLOPT_FOLLOWLOCATION, 0L);

  // Timeouts
  int64 timeout_ms = fetcher->timeout_ms();
  if (timeout_ms > 0) {
    curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(timeout_ms));
    curl_easy_setopt(curl_handle_, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(timeout_ms));
  }

  // Proxy
  if (!fetcher->proxy().empty()) {
    curl_easy_setopt(curl_handle_, CURLOPT_PROXY,
                     fetcher->proxy().c_str());
  }

  // SSL configuration
  if (fetcher->allow_https()) {
    if (fetcher->allow_self_signed() ||
        fetcher->allow_unknown_certificate_authority()) {
      curl_easy_setopt(curl_handle_, CURLOPT_SSL_VERIFYPEER, 0L);
    } else {
      curl_easy_setopt(curl_handle_, CURLOPT_SSL_VERIFYPEER, 1L);
    }
    curl_easy_setopt(curl_handle_, CURLOPT_SSL_VERIFYHOST, 2L);

    if (!fetcher->ssl_certificates_dir().empty()) {
      curl_easy_setopt(curl_handle_, CURLOPT_CAPATH,
                       fetcher->ssl_certificates_dir().c_str());
    }
    if (!fetcher->ssl_certificates_file().empty()) {
      curl_easy_setopt(curl_handle_, CURLOPT_CAINFO,
                       fetcher->ssl_certificates_file().c_str());
    } else if (!fetcher->ssl_certificates_dir().empty()) {
      curl_easy_setopt(curl_handle_, CURLOPT_CAINFO, "");
    }
  }

  // Handle request method and body
  FixUserAgent();
  RequestHeaders* req_headers = async_fetch_->request_headers();
  if (req_headers != nullptr) {
    RequestHeaders::Method method = req_headers->method();
    switch (method) {
      case RequestHeaders::kGet:
        // Default
        break;
      case RequestHeaders::kHead:
        curl_easy_setopt(curl_handle_, CURLOPT_NOBODY, 1L);
        break;
      case RequestHeaders::kPost:
        curl_easy_setopt(curl_handle_, CURLOPT_POST, 1L);
        if (!req_headers->message_body().empty()) {
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDSIZE,
                           static_cast<long>(req_headers->message_body().size()));
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDS,
                           req_headers->message_body().c_str());
        } else {
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDSIZE, 0L);
        }
        break;
      case RequestHeaders::kPut:
        curl_easy_setopt(curl_handle_, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!req_headers->message_body().empty()) {
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDSIZE,
                           static_cast<long>(req_headers->message_body().size()));
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDS,
                           req_headers->message_body().c_str());
        }
        break;
      case RequestHeaders::kDelete:
        curl_easy_setopt(curl_handle_, CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
      case RequestHeaders::kPatch:
        curl_easy_setopt(curl_handle_, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (!req_headers->message_body().empty()) {
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDSIZE,
                           static_cast<long>(req_headers->message_body().size()));
          curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDS,
                           req_headers->message_body().c_str());
        }
        break;
      case RequestHeaders::kPurge:
        curl_easy_setopt(curl_handle_, CURLOPT_CUSTOMREQUEST, "PURGE");
        break;
      default:
        // Use method_string() for any other methods
        curl_easy_setopt(curl_handle_, CURLOPT_CUSTOMREQUEST,
                         req_headers->method_string());
        break;
    }

    // Forward request headers, filtering hop-by-hop headers
    for (int i = 0; i < req_headers->NumAttributes(); ++i) {
      const GoogleString& name = req_headers->Name(i);
      const GoogleString& value = req_headers->Value(i);
      // Skip hop-by-hop and curl-managed headers
      if (StringCaseEqual(name, "Connection") ||
          StringCaseEqual(name, "Keep-Alive") ||
          StringCaseEqual(name, "Proxy-Connection") ||
          StringCaseEqual(name, "Transfer-Encoding") ||
          StringCaseEqual(name, "TE") ||
          StringCaseEqual(name, "Trailer") ||
          StringCaseEqual(name, "Upgrade") ||
          StringCaseEqual(name, "Content-Length")) {
        continue;
      }
      GoogleString header_line = StrCat(name, ": ", value);
      request_headers_list_ =
          curl_slist_append(request_headers_list_, header_line.c_str());
    }
  }

  // Accept-Encoding for gzip
  if (fetcher->fetch_with_gzip()) {
    curl_easy_setopt(curl_handle_, CURLOPT_ACCEPT_ENCODING, "gzip");
  }

  if (request_headers_list_ != nullptr) {
    curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, request_headers_list_);
  }

  return true;
}

void CurlFetch::Done(CURLcode result) {
  fetch_end_ms_ = timer_->NowMs();

  CurlFetchCompletionResult completion_result;
  if (result == CURLE_OK) {
    completion_result = CurlFetchCompletionResult::kSuccess;
    // Ensure headers are complete even if no body was received
    if (!headers_complete_) {
      headers_complete_ = true;
      async_fetch_->HeadersComplete();
    }
  } else if (result == CURLE_ABORTED_BY_CALLBACK) {
    completion_result = CurlFetchCompletionResult::kClientCancel;
    if (!headers_complete_) {
      async_fetch_->response_headers()->set_status_code(HttpStatus::kNotFound);
      headers_complete_ = true;
      async_fetch_->HeadersComplete();
    }
    message_handler_->Message(kWarning, "Fetch failed (%s): %s",
                              curl_easy_strerror(result), url_.c_str());
  } else if (result == CURLE_OPERATION_TIMEDOUT) {
    completion_result = CurlFetchCompletionResult::kTimeout;
    if (!headers_complete_) {
      async_fetch_->response_headers()->set_status_code(HttpStatus::kNotFound);
      headers_complete_ = true;
      async_fetch_->HeadersComplete();
    }
    message_handler_->Message(kWarning, "Fetch timed out: %s", url_.c_str());
  } else {
    completion_result = CurlFetchCompletionResult::kFailure;
    if (!headers_complete_) {
      async_fetch_->response_headers()->set_status_code(HttpStatus::kNotFound);
      headers_complete_ = true;
      async_fetch_->HeadersComplete();
    }
    // Track SSL certificate errors
    if (result == CURLE_SSL_CERTPROBLEM ||
        result == CURLE_SSL_CACERT ||
        result == CURLE_PEER_FAILED_VERIFICATION ||
        result == CURLE_SSL_ISSUER_ERROR) {
      fetcher_->IncrementCertErrors();
    }
    message_handler_->Message(kWarning, "Fetch failed (%s): %s",
                              curl_easy_strerror(result), url_.c_str());
  }

  bool success = (result == CURLE_OK);

  // Set X-Original-Content-Length if tracking is enabled
  if (fetcher_->track_original_content_length() &&
      !async_fetch_->response_headers()->Has(
          HttpAttributes::kXOriginalContentLength)) {
    async_fetch_->response_headers()->SetOriginalContentLength(
        bytes_received_);
  }

  // Report stats before calling Done on the async_fetch
  fetcher_->ReportCompletedFetchStats(this);
  fetcher_->ReportFetchSuccessStats(
      completion_result, async_fetch_->response_headers(), this);

  async_fetch_->Done(success);
  fetcher_->FetchComplete(this);
}

int64 CurlFetch::TimeDuration() const {
  if (fetch_end_ms_ > 0) {
    return fetch_end_ms_ - fetch_start_ms_;
  }
  return 0;
}

size_t CurlFetch::HeaderCallback(char* buffer, size_t size, size_t nmemb,
                                 void* userdata) {
  size_t total = size * nmemb;
  CurlFetch* fetch = static_cast<CurlFetch*>(userdata);

  if (fetch->headers_complete_) {
    return total;
  }

  // Reject responses with excessive total header size to prevent OOM.
  fetch->header_bytes_received_ += total;
  if (fetch->header_bytes_received_ > kMaxHeaderBytes) {
    fetch->message_handler_->Message(
        kWarning, "Response headers too large (>%zu bytes), aborting fetch for %s",
        kMaxHeaderBytes, fetch->url_.c_str());
    return 0;  // Returning 0 causes curl to abort with CURLE_WRITE_ERROR.
  }

  StringPiece line(buffer, total);
  // Strip trailing \r\n
  while (line.ends_with("\n") || line.ends_with("\r")) {
    line.remove_suffix(1);
  }

  if (line.empty()) {
    // Blank line signals end of headers. Get status code from curl.
    long response_code = 0;
    curl_easy_getinfo(fetch->curl_handle_, CURLINFO_RESPONSE_CODE,
                      &response_code);
    fetch->async_fetch_->response_headers()->set_status_code(
        static_cast<int>(response_code));
    fetch->headers_complete_ = true;
    fetch->async_fetch_->HeadersComplete();
  } else if (line.starts_with("HTTP/")) {
    // Status line - skip, we get the status code via CURLINFO_RESPONSE_CODE
  } else {
    // Regular header line: "Name: Value"
    size_t colon = line.find(':');
    if (colon != StringPiece::npos) {
      StringPiece name = line.substr(0, colon);
      StringPiece value = line.substr(colon + 1);
      // Strip leading whitespace from value
      while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
        value.remove_prefix(1);
      }
      fetch->async_fetch_->response_headers()->Add(name, value);
    }
  }

  return total;
}

size_t CurlFetch::WriteCallback(char* buffer, size_t size, size_t nmemb,
                                void* userdata) {
  size_t total = size * nmemb;
  CurlFetch* fetch = static_cast<CurlFetch*>(userdata);

  if (!fetch->headers_complete_) {
    fetch->headers_complete_ = true;
    fetch->async_fetch_->HeadersComplete();
  }

  fetch->bytes_received_ += total;

  // Reject responses that exceed the maximum body size to prevent OOM.
  if (fetch->bytes_received_ > kMaxResponseBodyBytes) {
    fetch->message_handler_->Message(
        kWarning, "Response body too large (>%zu bytes), aborting fetch for %s",
        kMaxResponseBodyBytes, fetch->url_.c_str());
    return 0;  // Returning 0 causes curl to abort with CURLE_WRITE_ERROR.
  }

  StringPiece data(buffer, total);
  fetch->async_fetch_->Write(data, fetch->message_handler_);

  return total;
}

void CurlFetch::FixUserAgent() {
  GoogleString user_agent;
  ConstStringStarVector v;
  RequestHeaders* request_headers = async_fetch_->request_headers();
  if (request_headers != nullptr &&
      request_headers->Lookup(HttpAttributes::kUserAgent, &v)) {
    for (int i = 0, n = v.size(); i < n; ++i) {
      if (i != 0) {
        user_agent += " ";
      }
      if (v[i] != nullptr) {
        user_agent += *(v[i]);
      }
    }
    request_headers->RemoveAll(HttpAttributes::kUserAgent);
  }
  if (user_agent.empty()) {
    user_agent += "CurlPagespeed";
  }
  GoogleString version =
      StrCat(" (", kModPagespeedSubrequestUserAgent,
             "/" MOD_PAGESPEED_FULL_VERSION_STRING "-" LASTCHANGE_STRING ")");
  if (!strings::EndsWith(StringPiece(user_agent), version)) {
    user_agent += version;
  }
  if (request_headers != nullptr) {
    request_headers->Add(HttpAttributes::kUserAgent, user_agent);
  }
}

}  // namespace net_instaweb

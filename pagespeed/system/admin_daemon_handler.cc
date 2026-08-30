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

#include "pagespeed/system/admin_daemon_handler.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/daemon_reader.h"

namespace net_instaweb {

namespace {

// The endpoint table: the ONLY source of upstream daemon paths.  The request
// leaf must match `leaf` exactly; the upstream request is then built from
// scratch (fixed method GET/HEAD, fixed path, no client query string, header,
// or body).  Adding a path here is a deliberate, reviewed act -- the daemon's
// socket transport is unauthenticated, so anything reachable through this
// table is reachable by anyone who can reach the admin console.
struct DaemonEndpoint {
  const char* leaf;           // exact request leaf, e.g. "health"
  const char* upstream_path;  // daemon API path, e.g. "/v1/health"
};

constexpr DaemonEndpoint kDaemonEndpoints[] = {
    {"health", "/v1/health"},
    {"stats", "/v1/stats"},
    {"cooldowns", "/v1/cache/cooldowns"},
};

static_assert(sizeof(kDaemonEndpoints) / sizeof(kDaemonEndpoints[0]) ==
              AdminDaemonHandler::kNumEndpoints);

}  // namespace

// DaemonProxyFetch captures the upstream daemon response and forwards status,
// content-type, and body (only those) to the original client fetch.
// Self-deletes on completion; modeled on LicenseProxyFetch in
// admin_license_handler.cc.  Named at namespace scope (not the anonymous
// namespace above) so AdminDaemonHandler can befriend it.
class DaemonProxyFetch : public StringAsyncFetch {
 public:
  DaemonProxyFetch(AsyncFetch* client_fetch, AdminDaemonHandler* handler,
                   int endpoint_index, bool head_request,
                   MessageHandler* message_handler)
      : StringAsyncFetch(client_fetch->request_context()),
        client_fetch_(client_fetch),
        handler_(handler),
        endpoint_index_(endpoint_index),
        head_request_(head_request),
        message_handler_(message_handler) {}

  void HandleDone(bool success) override {
    set_success(success);
    set_done(true);

    ResponseHeaders* client_headers = client_fetch_->response_headers();
    const int status = response_headers()->status_code();
    if (!success || status < 100) {
      // Upstream unreachable, timed out, or response over the size cap:
      // degrade to 502 so the console renders "daemon unreachable".
      client_headers->set_status_code(HttpStatus::kBadGateway);
      client_headers->Add(HttpAttributes::kContentType,
                          kContentTypeJson.mime_type());
      client_headers->Add(HttpAttributes::kCacheControl, "no-store");
      client_fetch_->Write("{\"error\":\"daemon unreachable\"}",
                           message_handler_);
    } else {
      // Forward ONLY status + body; no upstream response headers are copied.
      // The Content-Type is pinned to JSON rather than taken from upstream:
      // every daemon endpoint serves JSON, and the socket peer is untrusted
      // (a spoofed peer returning text/html would otherwise render in the
      // admin origin).
      client_headers->set_status_code(status);
      client_headers->Add(HttpAttributes::kContentType,
                          kContentTypeJson.mime_type());
      client_headers->Add(HttpAttributes::kCacheControl, "no-store");
      if (!head_request_) {
        client_fetch_->Write(buffer(), message_handler_);
      }
    }
    client_fetch_->Done(true);

    // Release the in-flight slot as the LAST handler access before
    // self-delete, so the handler's shutdown drain cannot observe the slot
    // released while we still touch handler state.
    handler_->ReleaseSlot(endpoint_index_);
    delete this;
  }

 private:
  AsyncFetch* client_fetch_;
  AdminDaemonHandler* handler_;
  int endpoint_index_;
  bool head_request_;
  MessageHandler* message_handler_;

  DaemonProxyFetch(const DaemonProxyFetch&) = delete;
  DaemonProxyFetch& operator=(const DaemonProxyFetch&) = delete;
};

AdminDaemonHandler::AdminDaemonHandler(DaemonReader* reader,
                                       MessageHandler* message_handler)
    : reader_(reader), message_handler_(message_handler) {}

AdminDaemonHandler::~AdminDaemonHandler() {
  // Wait for in-flight proxy fetches to complete so their HandleDone() never
  // touches a destroyed handler.  Bounded: the transport's upstream timeout
  // (5s) guarantees every fetch completes, so this is a short drain, not a
  // shutdown hang.  (Same pattern as ~AdminLicenseHandler.)
  static constexpr int64_t kDestructorTimeoutMs = 10000;
  static constexpr int64_t kPollIntervalMs = 10;
  int64_t waited_ms = 0;
  for (;;) {
    bool any_in_flight = false;
    for (int i = 0; i < kNumEndpoints; ++i) {
      if (in_flight_[i].load(std::memory_order_acquire)) {
        any_in_flight = true;
        break;
      }
    }
    if (!any_in_flight) {
      return;
    }
    if (waited_ms >= kDestructorTimeoutMs) {
      // Use fprintf instead of LOG() -- the logging sink may already be torn
      // down during process shutdown, causing a use-after-free crash.
      fprintf(stderr,
              "AdminDaemonHandler: timed out waiting for in-flight "
              "daemon fetches to complete during shutdown\n");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    waited_ms += kPollIntervalMs;
  }
}

bool AdminDaemonHandler::HandleRequest(StringPiece leaf, AsyncFetch* fetch) {
  int index = -1;
  for (int i = 0; i < kNumEndpoints; ++i) {
    if (leaf == kDaemonEndpoints[i].leaf) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    return false;  // Caller responds 404.
  }

  // GET/HEAD only: the daemon's socket transport is unauthenticated for
  // every method, so this proxy must never originate a state-changing
  // request.
  const RequestHeaders* request_headers = fetch->request_headers();
  const RequestHeaders::Method method = request_headers != nullptr
                                            ? request_headers->method()
                                            : RequestHeaders::kGet;
  if (method != RequestHeaders::kGet && method != RequestHeaders::kHead) {
    fetch->response_headers()->Add(HttpAttributes::kAllow, "GET, HEAD");
    WriteError(fetch, HttpStatus::kMethodNotAllowed,
               "method not allowed; daemon endpoints are read-only");
    return true;
  }

  // Per-endpoint in-flight slot: polling panels on different endpoints must
  // not 429 each other, but one endpoint allows exactly one upstream read at
  // a time.
  bool expected = false;
  if (!in_flight_[index].compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
    WriteError(fetch, 429,
               "a request for this daemon endpoint is already in flight");
    return true;
  }

  DaemonProxyFetch* proxy_fetch = new DaemonProxyFetch(
      fetch, this, index, method == RequestHeaders::kHead, message_handler_);
  // The upstream request is built from scratch: fixed method, fixed path
  // from the table, no client query string, headers, or body.
  proxy_fetch->request_headers()->set_method(method);

  if (reader_ == nullptr) {
    // No daemon transport on this port: behave like an unreachable daemon.
    proxy_fetch->HandleDone(false);
    return true;
  }
  reader_->Get(kDaemonEndpoints[index].upstream_path, proxy_fetch);
  return true;
}

void AdminDaemonHandler::WriteError(AsyncFetch* fetch, int status_code,
                                    StringPiece error) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->set_status_code(status_code);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  headers->Add(HttpAttributes::kCacheControl, "no-store");
  GoogleString json = StrCat("{\"error\":\"", JsonEscape(error), "\"}");
  fetch->Write(json, message_handler_);
  fetch->Done(true);
}

}  // namespace net_instaweb

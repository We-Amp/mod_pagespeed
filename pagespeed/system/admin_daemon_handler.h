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

#ifndef PAGESPEED_SYSTEM_ADMIN_DAEMON_HANDLER_H_
#define PAGESPEED_SYSTEM_ADMIN_DAEMON_HANDLER_H_

#include <atomic>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class AsyncFetch;
class DaemonReader;
class MessageHandler;

// Handles the admin console's /v1/daemon/* endpoints: a read-only proxy to
// the optimizer daemon's management API.
//
// The proxy is read-only BY CONSTRUCTION, because the daemon's socket
// transport is unauthenticated for every method.  The invariants are enforced
// here, not configured:
//  - GET/HEAD only; any other method gets 405.
//  - The upstream path comes from a compile-time table (exact leaf match),
//    never from the request; no client query string, header, or request body
//    is forwarded upstream.
//  - The response copies upstream status + body only; the Content-Type is
//    pinned to JSON (all daemon endpoints serve JSON, and the socket peer is
//    untrusted) and no upstream response headers are forwarded.
//  - One in-flight request per endpoint (a second concurrent request gets
//    429); the transport enforces a 5s upstream timeout and a 1 MiB response
//    cap.
//  - Cache-Control: no-store on every response; upstream failure (including
//    "no socket configured") degrades to 502 with a short JSON error body so
//    the console panels can render "daemon unreachable".
class AdminDaemonHandler {
 public:
  // `reader` is borrowed and may be nullptr (no daemon transport on this
  // port): every endpoint then reports the daemon unreachable (502).
  AdminDaemonHandler(DaemonReader* reader, MessageHandler* message_handler);
  ~AdminDaemonHandler();

  // Handles one /v1/daemon/<leaf> request; `leaf` is the exact remainder of
  // the URL path after "/v1/daemon/".  Returns false if `leaf` is not in the
  // endpoint table (the caller responds 404 and must complete `fetch`);
  // otherwise the response has been completed or started asynchronously and
  // the caller must NOT touch `fetch` again.
  bool HandleRequest(StringPiece leaf, AsyncFetch* fetch);

  // Number of entries in the endpoint table.
  static constexpr int kNumEndpoints = 3;

 private:
  friend class DaemonProxyFetch;

  // Releases the per-endpoint in-flight slot.  Called by the proxy fetch as
  // its last handler access before self-deletion.
  void ReleaseSlot(int index) {
    in_flight_[index].store(false, std::memory_order_release);
  }

  // Writes a short JSON error response with Cache-Control: no-store and
  // completes the fetch.
  void WriteError(AsyncFetch* fetch, int status_code, StringPiece error);

  DaemonReader* reader_;
  MessageHandler* message_handler_;
  std::atomic<bool> in_flight_[kNumEndpoints]{};

  AdminDaemonHandler(const AdminDaemonHandler&) = delete;
  AdminDaemonHandler& operator=(const AdminDaemonHandler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_ADMIN_DAEMON_HANDLER_H_

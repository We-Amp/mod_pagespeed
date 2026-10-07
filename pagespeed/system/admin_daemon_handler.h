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
class QueryParams;
class ResponseHeaders;

// Handles the admin console's /v1/daemon/* endpoints: a read-only proxy to
// the optimizer daemon's management API.
//
// The proxy is read-only BY CONSTRUCTION, because the daemon's socket
// transport is unauthenticated for every method.  The invariants are
// enforced here, not configured:
//  - GET/HEAD only; any other method gets 405.
//  - The upstream path comes from a compile-time table (exact leaf match;
//    a leaf is one or two path segments, e.g. "health" or "cache/urls"),
//    never from the request.
//  - Query parameters pass only through the table's per-endpoint
//    allow-list: an unknown or not-allow-listed parameter is a 400, and
//    the upstream query is rebuilt by this module from validated values
//    -- the raw client query string, like client headers and bodies,
//    never reaches the daemon.
//  - Leaves whose data spans every virtual host (the optimizer's cache
//    index and variants, and its process-wide log) answer 403
//    "whole_server_console_only" on a per-virtual-host console, before
//    any parameter is looked at.
//  - A leaf served on a per-virtual-host console whose answer names other
//    sites is narrowed by the module before anything is sent: stats keeps
//    only this site's row of the optimizer's serve savings per host (the
//    rest folded into "other"), cooldowns only this site's entries.  The
//    narrowed answer is rebuilt from the module's own strict reading of
//    the optimizer's JSON (daemon_site_filter.h); an answer it cannot
//    read is 502 "daemon_unreachable", an upstream error status carries
//    {"error":"daemon_error"} instead of the optimizer's body.  The
//    whole-server console's answers are forwarded unchanged.
//  - Those same leaves also answer 403 "cross_site_request" when the
//    request carries Sec-Fetch-Site set to anything other than
//    "same-origin" or "none" -- a cross-site page must never be able
//    to trigger the read at all, even though it can never see the
//    response.  A request without the header (a non-browser client,
//    e.g. monitoring) is unaffected, and so is every other leaf.
//  - The response copies upstream status + body only; the Content-Type
//    is pinned to JSON and no upstream response headers are forwarded.
//    The one byte-serving leaf (cache/content) instead passes a
//    media-type gate -- exactly image/png|jpeg|gif|webp|avif, parameters
//    dropped -- serves bytes only on a gated upstream 200, answers every
//    other upstream status itself, and carries a sandboxing CSP on every
//    response, success or error, and a module-built inline filename on
//    served bytes.
//  - Each endpoint admits a fixed number of concurrent upstream reads --
//    one, and two on cache/content for parallel image loads -- beyond
//    that, 429.  cache/content's two are counted across every handler
//    in the server worker process (there is one handler per server
//    context), so its buffered bytes stay within 2 x the content cap
//    (32 MiB) per server worker process.  The transport enforces a 5s
//    upstream timeout and a
//    per-read response cap (1 MiB JSON, 16 MiB content) and reports why
//    a read failed; only an observed over-cap body is
//    "content_too_large".
//  - Every response is Cache-Control: no-store, nosniff and
//    Cross-Origin-Resource-Policy: same-origin, with a short JSON error
//    body on every non-2xx-forwarded branch so the console panels can
//    render why: no transport configured -> 503 "daemon_not_configured";
//    transport failure/timeout -> 502 "daemon_unreachable"; an
//    over-cap answer -> 502 "content_too_large" on cache/content,
//    "response_too_large" on logs, "daemon_unreachable" elsewhere;
//    upstream 404 on a per-URL cache leaf (a route every supported
//    optimizer serves) -> 404 "not_in_index", the daemon's body dropped;
//    upstream 404 on any other table endpoint (the running optimizer
//    predates it) -> 501 "endpoint_unsupported_by_daemon";
//    query-parameter violations -> 400 with a reason code naming the
//    parameter.
class AdminDaemonHandler {
 public:
  // `reader` is borrowed and may be nullptr (no daemon transport on this
  // port): every endpoint then reports 503 "daemon_not_configured".
  AdminDaemonHandler(DaemonReader* reader, MessageHandler* message_handler);
  ~AdminDaemonHandler();

  // Handles one /v1/daemon/<leaf> request; `leaf` is the one- or
  // two-segment remainder of the URL path directly after the final
  // "v1/daemon" segment pair (so "cache/urls" is one leaf).
  // `query_params` are the request's query parameters (checked against
  // the endpoint's allow-list), `is_global` is the console's scope, and
  // `own_serve_host` is the host this request's own site records its
  // serves under (VouchedServeHost; "" when it has none) -- on a
  // per-virtual-host console the leaves whose answers name other sites
  // are narrowed to it.
  // Returns false if `leaf` is not in the endpoint table (the caller
  // responds 404 and must complete `fetch`); otherwise the response has
  // been completed or started asynchronously and the caller must NOT
  // touch `fetch` again.
  bool HandleRequest(StringPiece leaf, const QueryParams& query_params,
                     bool is_global, StringPiece own_serve_host,
                     AsyncFetch* fetch);

  // Number of entries in the endpoint table.
  static constexpr int kNumEndpoints = 7;

  // The number of cache/content reads admitted across every handler in
  // this process (for tests: the process-wide slot must drain to 0).
  static int ContentReadsInFlightForTesting();

  // Adds the three headers every daemon-proxy response carries (see the
  // class comment above): Cache-Control: no-store, private;
  // X-Content-Type-Options: nosniff; and Cross-Origin-Resource-Policy:
  // same-origin. Exposed so the admin_site.cc dispatch can apply them to
  // the 404 it writes when HandleRequest() returns false for a leaf this
  // table does not know -- the one daemon-proxy response this class does
  // not compose itself.
  static void AddProxyResponseHeaders(ResponseHeaders* headers);

 private:
  friend class DaemonProxyFetch;

  // Releases one of the endpoint's in-flight slots (and, for the
  // byte-serving leaf, its process-wide slot).  Called by the proxy
  // fetch as its last handler access before self-deletion.
  void ReleaseSlot(int index);

  // Writes a short JSON error response with Cache-Control: no-store (and,
  // for the byte-serving leaf, its sandboxing CSP) and completes the
  // fetch.
  void WriteError(AsyncFetch* fetch, int status_code, StringPiece error,
                  bool content_leaf = false);

  // Writes a 400 JSON error body naming the offending query parameter
  // ({"error":"<code>","parameter":"<name>"}, the name sanitised for
  // echo) and completes the fetch.
  void WriteParamError(AsyncFetch* fetch, StringPiece code, StringPiece param,
                       bool content_leaf = false);

  DaemonReader* reader_;
  MessageHandler* message_handler_;
  std::atomic<int> in_flight_[kNumEndpoints]{};

  AdminDaemonHandler(const AdminDaemonHandler&) = delete;
  AdminDaemonHandler& operator=(const AdminDaemonHandler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_ADMIN_DAEMON_HANDLER_H_

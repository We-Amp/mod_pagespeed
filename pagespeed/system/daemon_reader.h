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

#ifndef PAGESPEED_SYSTEM_DAEMON_READER_H_
#define PAGESPEED_SYSTEM_DAEMON_READER_H_

#include <cstddef>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class AsyncFetch;

// Default unix socket path of the optimizer daemon's management API, matching
// the daemon's packaged default for --api-socket.
inline constexpr char kDefaultDaemonApiSocketPath[] =
    "/run/pagespeed-optimizer/api.sock";

// Per-read response body caps, stated by the caller (no default: the right
// size depends on the endpoint).  JSON management answers are far under
// 1 MiB; a cached image original can be several MiB.  An over-cap body is a
// failure (DaemonReadFailure::kTooLarge), never a truncation.
inline constexpr size_t kMaxJsonResponseBytes = 1024 * 1024;
inline constexpr size_t kMaxContentResponseBytes = 16 * 1024 * 1024;

// Why a daemon read failed, reported by the reader so callers never have to
// guess from a status code.
enum class DaemonReadFailure {
  kNone,          // the read succeeded
  kDisabled,      // the reader has no transport (empty socket path)
  kTimeout,       // no complete answer within the reader's timeout
  kDisconnected,  // connect failed or the connection dropped mid-answer
  kTooLarge,      // the body exceeded the caller's max_response_bytes
};

// Read-only seam between the module's admin console and the optimizer
// daemon's management API.
//
// Implementations issue a GET for `daemon_path` (e.g. "/v1/health") and
// complete `fetch` exactly once with the upstream status code, content type,
// and body.  Only read methods exist, deliberately: the daemon's socket
// transport is unauthenticated for every method, so a writable reader would
// be an unauthenticated control channel.  Do not add write methods without a
// separate, deliberate decision.
//
// The seam exists so another transport (e.g. TCP) can be added later without
// touching callers; the only implementation at present is UdsDaemonReader.
class DaemonReader {
 public:
  virtual ~DaemonReader() = default;

  // Starts an asynchronous read of `daemon_path`, an absolute daemon API
  // path (with an optional query the caller built from validated values)
  // from the caller's allow-list -- never raw request-derived input.
  // The read fails with kTooLarge if the body exceeds `max_response_bytes`;
  // no byte past the cap reaches `fetch`.
  // `fetch` carries the request method (GET, or HEAD for a headers-only
  // read) and receives the upstream response; its Done() is called exactly
  // once, synchronously or asynchronously.  `*failure` is written exactly
  // once, before Done(): kNone iff Done(true), else why the read failed.
  // `failure` must stay valid until then.
  virtual void Get(StringPiece daemon_path, size_t max_response_bytes,
                   AsyncFetch* fetch, DaemonReadFailure* failure) = 0;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_READER_H_

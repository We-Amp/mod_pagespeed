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

#ifndef PAGESPEED_NGINX_NGX_DAEMON_SERVE_NOTIFY_H_
#define PAGESPEED_NGINX_NGX_DAEMON_SERVE_NOTIFY_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_serve_arm.h"

namespace net_instaweb {

class Variable;

// The serve path's three conditional notifications, carried off the event
// thread to the record arm's one-worker sequence.
//
// A notification is a socket call to another process, so on this port it
// never runs on the event thread: the closure below is POSTED to the same
// sequence the record completions drain on, shares their queue caps, and
// sends nothing when there is no sequence to post to.
//
// Deliberately nginx-free so the closure is unit tested standalone
// (//test/pagespeed/nginx:ngx_daemon_serve_notify_test).  Nothing here
// knows what an ngx_http_request_t is.

// Which notification one closure carries: the fallback hit's re-notify for
// the variant the client's mask really names, the age-expired
// fall-through's origin-refreshed sentinel, or the ask for an optimized
// copy that went missing behind a stored-original serve.
enum class DaemonServeNotifyKind {
  kFallbackRenotify,
  kOriginRefreshed,
  kLostCopy
};

// One notification, queued.  Owns COPIES of everything the three shared
// notify functions read across the thread boundary -- the request's url,
// hostname and scheme (those three DaemonServeRequest fields are borrowing
// StringPieces), the two option-context halves, and the handful of decision
// scalars -- and NEVER a DaemonServeDecision: the one reconstituted in
// Run() carries scalars only, its body an empty StringPiece by construction,
// so nothing borrowed from the dead reader can be touched.
class DaemonServeNotify : public Function {
 public:
  // `abi` and the two counters are borrowed.  The counters live in shared
  // memory.  The abi belongs to the server context's adapter: the exit hook
  // shuts the pool down -- and any still-queued closure is cancelled, which
  // sends nothing -- before the volume close and long before the adapter
  // and its library die, so no Run() can dereference a dead abi.  The
  // socket path is copied.
  DaemonServeNotify(DaemonServeNotifyKind kind, const DaemonAbi* abi,
                    StringPiece socket_path, StringPiece url,
                    StringPiece hostname, StringPiece scheme,
                    const DaemonServeDecision& decision,
                    StringPiece option_context, StringPiece option_signature,
                    Variable* notified, Variable* notify_failed);
  ~DaemonServeNotify() override;

  void Run() override;
  // Shutdown with the notification still queued: it is never sent.  Nothing
  // runs inline, on any path.
  void Cancel() override;

 private:
  const DaemonServeNotifyKind kind_;
  const DaemonAbi* abi_;  // borrowed; see the constructor comment
  GoogleString socket_path_;
  GoogleString url_;
  GoogleString hostname_;
  GoogleString scheme_;
  // The only decision fields the three shared functions read (verified
  // against daemon_serve_arm.cc: no other field, and never `body`).
  DaemonServeVerdict verdict_;
  bool fallback_hit_;
  bool stale_variant_expired_by_age_;
  bool lost_optimized_copy_;
  bool vary_accept_origin_declared_;
  int ps_content_type_;
  uint32_t client_mask_;
  GoogleString option_context_;
  GoogleString option_signature_;
  Variable* notified_;       // borrowed; shared memory
  Variable* notify_failed_;  // borrowed; shared memory

  DaemonServeNotify(const DaemonServeNotify&) = delete;
  DaemonServeNotify& operator=(const DaemonServeNotify&) = delete;
};

// Posts one notification onto `sequence` -- the record arm's one-worker
// sequence -- and NEVER calls anything inline.  When the sequence does not
// exist, or the queue caps shared with the record completions turn the
// closure away, nothing is posted and `dropped` (when given) moves once.
// `request` and `decision` are read only to take the copies the closure
// owns; `abi`, the counters and `dropped` stay borrowed as above.
void PostDaemonServeNotify(QueuedWorkerPool::Sequence* sequence,
                           DaemonServeNotifyKind kind, const DaemonAbi* abi,
                           StringPiece socket_path,
                           const DaemonServeRequest& request,
                           const DaemonServeDecision& decision,
                           StringPiece option_context,
                           StringPiece option_signature, Variable* notified,
                           Variable* notify_failed, Variable* dropped);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_DAEMON_SERVE_NOTIFY_H_

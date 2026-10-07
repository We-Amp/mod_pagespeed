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

#include "pagespeed/nginx/ngx_daemon_serve_notify.h"

#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/nginx/ngx_daemon_record_completion.h"

namespace net_instaweb {

DaemonServeNotify::DaemonServeNotify(
    DaemonServeNotifyKind kind, const DaemonAbi* abi, StringPiece socket_path,
    StringPiece url, StringPiece hostname, StringPiece scheme,
    const DaemonServeDecision& decision, StringPiece option_context,
    StringPiece option_signature, Variable* notified, Variable* notify_failed)
    : kind_(kind),
      abi_(abi),
      verdict_(decision.verdict),
      fallback_hit_(decision.fallback_hit),
      stale_variant_expired_by_age_(decision.stale_variant_expired_by_age),
      lost_optimized_copy_(decision.lost_optimized_copy),
      vary_accept_origin_declared_(decision.vary_accept_origin_declared),
      ps_content_type_(decision.ps_content_type),
      client_mask_(decision.client_mask),
      notified_(notified),
      notify_failed_(notify_failed) {
  socket_path.CopyToString(&socket_path_);
  url.CopyToString(&url_);
  hostname.CopyToString(&hostname_);
  scheme.CopyToString(&scheme_);
  option_context.CopyToString(&option_context_);
  option_signature.CopyToString(&option_signature_);
}

DaemonServeNotify::~DaemonServeNotify() {
  // The queue reservation is released whichever way the closure leaves the
  // pool -- ran, cancelled, or destroyed still queued.  A closure only ever
  // exists after a successful reservation (see PostDaemonServeNotify).
  DaemonCompletionExited(0);
}

void DaemonServeNotify::Run() {
  // Reconstitute just enough request and decision: owned strings, scalar
  // fields, and a body that is empty by construction.  The shared functions
  // re-apply their own gates (verdict, flag, both context halves), so a
  // decision that does not call for the notification still sends nothing.
  DaemonServeRequest request;
  request.url = url_;
  request.hostname = hostname_;
  request.scheme = scheme_;
  DaemonServeDecision decision;
  decision.verdict = verdict_;
  decision.fallback_hit = fallback_hit_;
  decision.stale_variant_expired_by_age = stale_variant_expired_by_age_;
  decision.lost_optimized_copy = lost_optimized_copy_;
  decision.vary_accept_origin_declared = vary_accept_origin_declared_;
  decision.ps_content_type = ps_content_type_;
  decision.client_mask = client_mask_;
  switch (kind_) {
    case DaemonServeNotifyKind::kFallbackRenotify:
      DaemonServeFallbackRenotify(*abi_, socket_path_, request, decision,
                                  option_context_, option_signature_, notified_,
                                  notify_failed_);
      break;
    case DaemonServeNotifyKind::kOriginRefreshed:
      DaemonServeOriginRefreshedNotify(*abi_, socket_path_, request, decision,
                                       option_context_, option_signature_,
                                       notified_, notify_failed_);
      break;
    case DaemonServeNotifyKind::kLostCopy:
      DaemonServeLostCopyNotify(*abi_, socket_path_, request, decision,
                                option_context_, option_signature_, notified_,
                                notify_failed_);
      break;
  }
}

void DaemonServeNotify::Cancel() {}

void PostDaemonServeNotify(QueuedWorkerPool::Sequence* sequence,
                           DaemonServeNotifyKind kind, const DaemonAbi* abi,
                           StringPiece socket_path,
                           const DaemonServeRequest& request,
                           const DaemonServeDecision& decision,
                           StringPiece option_context,
                           StringPiece option_signature, Variable* notified,
                           Variable* notify_failed, Variable* dropped) {
  if (sequence == nullptr) {
    // No sequence to post to: nothing is called inline, and the drop is
    // counted so a missing pool is distinguishable from a quiet substrate.
    if (dropped != nullptr) {
      dropped->Add(1);
    }
    return;
  }
  // The reservation first: the closure's destructor releases it, so the
  // closure must exist only after a reservation was taken.  Notifications
  // draw on the HALF-items budget (kDaemonNotifyQueueMaxItems), so a
  // fallback storm can never starve record completions out of the queue.
  if (!TryQueueDaemonNotification(dropped)) {
    return;
  }
  sequence->Add(new DaemonServeNotify(
      kind, abi, socket_path, request.url, request.hostname, request.scheme,
      decision, option_context, option_signature, notified, notify_failed));
}

}  // namespace net_instaweb

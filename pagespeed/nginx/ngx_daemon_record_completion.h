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

#ifndef PAGESPEED_NGINX_NGX_DAEMON_RECORD_COMPLETION_H_
#define PAGESPEED_NGINX_NGX_DAEMON_RECORD_COMPLETION_H_

#include <functional>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"

namespace net_instaweb {

class DaemonAdapter;
class IproRecorder;
class ResponseHeaders;
class Statistics;
class Variable;

// The completion half of the daemon record arm for nginx.
//
// A recorder finishes with DoneAndSetHeaders, which opens a write on the
// daemon's shared volume, streams the buffered original and may notify the
// optimizer -- work that blocks, and therefore may not run on nginx's event
// thread.  This translation unit owns everything about getting that one call
// onto a PSOL low-priority-pool sequence instead: the closure that carries
// it, the process-wide caps that keep the queue bounded, the counter for
// completions the caps turned away, and the time latch that keeps a missing
// volume handle from being re-attempted per request.
//
// Deliberately nginx-free so it can be unit tested standalone
// (//test/pagespeed/nginx:ngx_daemon_record_completion_test).  Nothing here
// knows what an ngx_http_request_t is.

// Name of the nginx-side counter for record completions the queue caps
// turned away.  Registered through ps_daemon_record_completion_init_stats, which
// the port's factory InitStats runs for EVERY statistics object before that
// object is initialised -- registering later, against an already-frozen
// shared-memory segment, is a fatal error.  Callers look the Variable up
// from the statistics object at hand; there is no process-wide cached
// pointer, because there are several statistics objects.
extern const char kIproDaemonRecordDropped[];

// Name of the nginx-side counter for serve-path NOTIFICATIONS (the fallback
// re-notify and the origin-refreshed sentinel) that had no sequence to post
// to or were turned away by the shared queue caps.  A dropped notification
// is not a dropped record, so it does not share kIproDaemonRecordDropped's
// meaning; it shares the same registration path.
extern const char kIproDaemonNotifyDropped[];

// The queue caps.  Bytes bound the memory parked in not-yet-run completions
// (each closure holds one recorder, which holds one buffered response body);
// items bound the backlog a single draining worker can be made to sit
// behind: the pool is one thread, so every queued item ahead of a
// completion is head-of-line latency that completion did not need, and a
// long queue is also a long drain at worker shutdown.  64 bounds both
// windows; the byte cap remains the real memory bound.  A completion that
// does not fit is finished inline as incomplete -- the response to the
// client is unaffected, the recording is simply not kept.
//
// NOTIFICATIONS share the items dimension but only up to HALF of it
// (kDaemonNotifyQueueMaxItems).  The policy, and why: a record completion
// carries a whole response body of work and a serve-path notification
// carries none; both losses heal themselves (a missed recording re-records
// on the next request, a refused notification re-asks on the next fallback
// hit or age-expired fall-through).  So the recording always wins the upper
// half of the cap: a fallback storm on an Accept-negotiating origin can
// never starve the recordings out of the queue.  A notification refused
// this way is counted on ipro_daemon_notify_dropped.
constexpr int64 kDaemonCompletionQueueMaxBytes = 64 * 1024 * 1024;
constexpr int kDaemonCompletionQueueMaxItems = 64;
constexpr int kDaemonNotifyQueueMaxItems = kDaemonCompletionQueueMaxItems / 2;

// At most one RecordCache() open attempt is posted per this interval when
// the health verdict says the daemon is usable but no volume handle is open
// yet.  RecordCache() has its own retry schedule; this latch only keeps a
// busy server from queueing an open per request.
constexpr int64 kDaemonOpenAttemptIntervalMs = 1000;

// Registers the drop counter on one statistics object.  Must run before
// that object is initialised; the port's factory InitStats is the call
// site.  A null statistics is accepted and registers nothing.
void ps_daemon_record_completion_init_stats(Statistics* statistics);

// Reserves queue room for one completion carrying `body_bytes` buffered
// bytes.  Returns false -- after counting a drop on `dropped`, when given --
// when either cap would be exceeded, or when the single body alone is over
// the byte cap.  On true the reservation is held until
// DaemonCompletionExited runs, which the completion's destructor guarantees
// on both the Run and the Cancel path.
bool TryQueueDaemonCompletion(int64 body_bytes, Variable* dropped);

// Reserves queue room for one serve-path NOTIFICATION: no body, so only the
// items dimension applies, and only up to kDaemonNotifyQueueMaxItems (half
// the item cap -- the policy is stated at the cap constant).  Returns false
// -- after counting a drop on `dropped`, when given -- when the
// notification budget is exhausted.  Released by the closure's destructor
// through the same DaemonCompletionExited.
bool TryQueueDaemonNotification(Variable* dropped);

// Releases one reservation.  Called by DaemonRecordCompletion's destructor;
// not for callers.
void DaemonCompletionExited(int64 body_bytes);

// True at most once per kDaemonOpenAttemptIntervalMs across the process.
bool ShouldPostDaemonOpenAttempt();

// Posts one RecordCache() open attempt for `adapter` onto `sequence`.  Both
// are borrowed.  The attempt's result is deliberately ignored: a null handle
// is the adapter's normal not-yet state, not an error this path reports.
void PostDaemonOpenAttempt(QueuedWorkerPool::Sequence* sequence,
                           DaemonAdapter* adapter);

// Clock seam for the open-attempt latch.  Test-only; passing an empty
// function restores the production steady clock.
using DaemonNowMsFn = std::function<int64()>;
void SetDaemonNowMsForTesting(DaemonNowMsFn fn);

// Zeroes the queue counters and the latch.  Test-only: not safe against
// live traffic.
void ResetDaemonCompletionQueueForTesting();

// One recorder's finish, carried to the pool.  Owns the final response
// headers (the recorder does not take them) and, until it runs, the
// recorder.  The pool deletes the closure after Run() or Cancel(), and the
// destructor releases the queue reservation, so the caps hold whichever way
// the closure leaves the pool.
class DaemonRecordCompletion : public Function {
 public:
  // `recorder` and `headers` are owned.  `complete` is the response's
  // last_buf: false finishes the recording as truncated, which stores
  // nothing.
  DaemonRecordCompletion(IproRecorder* recorder, ResponseHeaders* headers,
                         bool complete, int64 body_bytes);
  ~DaemonRecordCompletion() override;

  void Run() override;
  // Shutdown with the completion still queued: the recording is finished as
  // incomplete, so the recorder is still destroyed exactly once and before
  // the volume handle it would write through can close.
  void Cancel() override;

 private:
  IproRecorder* recorder_;
  ResponseHeaders* headers_;
  bool complete_;
  int64 body_bytes_;

  DaemonRecordCompletion(const DaemonRecordCompletion&) = delete;
  DaemonRecordCompletion& operator=(const DaemonRecordCompletion&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_DAEMON_RECORD_COMPLETION_H_

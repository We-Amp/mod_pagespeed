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

// Zero-copy ALIASED serve, Apache sink (CycloneZeroCopyServe).
//
// A custom apr_bucket type whose window aliases a Cyclone memory-mapped
// cache region, pinned by a refcounted MappedSharedString read-handle
// reference.  Cyclone read leases defend the borrowed region against
// NORMAL cache wraps only while freshly stamped, and a ceiling-FORCED
// wrap overwrites pinned regions regardless, so the bucket enforces the
// lease barrier discipline through the bucket API itself:
//
//  * read() is the per-send barrier.  httpd's core output filter calls
//    apr_bucket_read on every pending bucket to rebuild its writev iovecs
//    after every poll wait (send_brigade_nonblocking is re-entered from
//    the EAGAIN/poll loop in ap_core_output_filter), so an intent-checked
//    lease revalidation here runs in the same stack, microseconds before
//    the kernel copies the bytes.  While the lease is provably live and no
//    forced wrap is imminent the read returns the aliased pointer;
//    otherwise it de-aliases in place (below) or fails the serve closed.
//
//  * setaside() RETAINS BY COPY.  A bucket parked past the current filter
//    pass (deferred core-filter writes via ap_save_brigade, mod_http2
//    beams, async MPM write completion) would outlive any lease guarantee,
//    so it morphs into an owned heap bucket, with a copy-then-verify:
//    the borrow is re-checked AFTER the memcpy, because a forced wrap
//    ignores the lease and can race the copy.
//
//  * destroy() drops this bucket's reference; the last reference releases
//    the pin (and with it the Cyclone read handle).  split()/copy() share
//    the refcounted pin, so byterange-style re-slicing stays safe: every
//    sibling runs its own barrier on read.
//
// Failure posture: a kTorn verdict (the epoch moved, the borrowed bytes
// may have been overwritten) fails the serve closed -- the caller aborts
// the connection rather than completing the Content-Length promise with a
// corrupt-but-complete body.  Any other doubt (wrap in flight, leases off,
// forced-wrap deadline near) degrades to a verified copy, never a reset.

#ifndef PAGESPEED_APACHE_APACHE_MMAP_BUCKET_H_
#define PAGESPEED_APACHE_APACHE_MMAP_BUCKET_H_

#include "apr_buckets.h"  // NOLINT
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class Variable;

// Read-barrier forced-wrap margin: a bucket read keeps aliasing
// only while a ceiling-forced wrap -- which ignores the read lease -- is at
// least this far off.  The exposure the margin must cover is one
// nonblocking writev burst issued in the same stack as the read
// (microseconds), so 1s is an enormous safety factor while still far under
// the forced-wrap ceiling: an uncontended serve stays aliased its whole
// life.  Emit-time admission (ApacheFetch::WriteMapped) uses a strictly
// LARGER margin so a serve is never admitted for aliasing only to be
// de-aliased by the very first read.
constexpr uint64_t kMmapAliasReadBarrierMarginNs =
    1000ULL * 1000 * 1000;  // 1 s

// Creates a bucket whose window aliases 'span', which must point into the
// mapped region pinned by 'pin' (is_mapped() and carrying the lease
// renew/force-wrap hooks).  The bucket holds its own reference on the pin.
// 'copied_out_stat' / 'renew_fail_stat' may be null; they count de-alias
// copies and torn-borrow serve failures.  Returns nullptr on allocation
// failure.
apr_bucket* CreateMmapAliasBucket(const StringPiece& span,
                                  const MappedSharedString& pin,
                                  Variable* copied_out_stat,
                                  Variable* renew_fail_stat,
                                  apr_bucket_alloc_t* list);

// True while 'b' is (still) the aliasing bucket type; false after a
// copy-out morphed it into a heap bucket.  Exposed for tests.
bool IsMmapAliasBucket(const apr_bucket* b);

// True if 'b' was poisoned by a failed setaside copy-out (torn borrow or
// allocation failure discovered while httpd's deferred-write path was
// parking it).  A poisoned bucket's read() always fails, so the serve
// aborts at the next send attempt -- the point where the core output
// filter genuinely honors errors.  Exposed for tests.
bool IsTornAliasBucket(const apr_bucket* b);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_MMAP_BUCKET_H_

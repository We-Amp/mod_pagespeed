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

// Decision core for the IIS zero-copy aliased serve (CycloneZeroCopyServe,
// the design record).  Kept free of IIS/Windows headers (pure logic) so the safety
// rules it encodes are unit-testable on every platform, following the
// iis_proxy_fetch_completion.h pattern.
//
// Model: the '.pagespeed.' rewritten-resource cache hit hands the port a
// StringPiece aliasing a Cyclone mmap region plus a MappedSharedString pin
// (read-lease keep-alive with the design record renew / force-wrap-deadline
// hooks).  The IIS sink serves the region as a sequence of bounded chunks,
// each submitted with WriteEntityChunkByReference + an ASYNC flush; the
// flush completion is IIS's authoritative "HTTP.sys is done reading this
// buffer" barrier signal.  Between chunks the driver re-probes the lease
// (RenewLeaseStrict re-stamps it, so normal wraps stay blocked while the
// client keeps up) and this planner decides what happens next:
//
//  * kAliasChunk  -- lease provably live (kOk) and NO deferred-wrap
//    pressure: point HTTP.sys at the mmap bytes for the next chunk.  A
//    ceiling-forced wrap ignores leases (it fires on the per-stripe clock,
//    volume-side), and a chunk's in-flight window is client-paced and
//    unbounded, so no time margin can prove the window closes before a
//    pending force fires; therefore ANY finite force-wrap deadline
//    (NsUntilForcedWrap != UINT64_MAX) disqualifies aliasing outright.
//  * kCopyTail    -- copy the whole remainder into request-owned memory and
//    finish from the copy.  Chosen when the lease state is not kOk
//    (kCopyNow: wrap in flight, region still intact; kLeasesOff: no
//    protection -- copy, never abort), when the stripe shows deferred-wrap
//    pressure, or when the remainder is small.  The caller MUST apply
//    copy-then-verify: memcpy first, re-probe the lease AFTER, and abort on
//    kTorn -- never check-then-copy (a forced wrap can race the memcpy).
//  * kAbort       -- the epoch moved (kTorn): the borrowed region has been
//    overwritten and the remaining source bytes are unrecoverable.  The
//    caller must reset the connection so the client never sees a
//    complete-but-corrupt body.  (Degrading to a copy is impossible here:
//    correct bytes can no longer be produced.)
//
// The FINAL kTailCopyBytes of the body are never aliased: an aliased
// chunk's tornness is only detectable at its flush completion, and for the
// last body bytes that completion IS body-complete at the client -- too
// late to abort.  Serving the tail from a verified copy guarantees that a
// complete 200 body implies every aliased chunk passed its post-exposure
// epoch verification.

#ifndef PAGESPEED_IIS_IIS_ZEROCOPY_SERVE_H_
#define PAGESPEED_IIS_IIS_ZEROCOPY_SERVE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "pagespeed/kernel/base/mapped_shared_string.h"

namespace net_instaweb {

class IisZeroCopyServePlanner {
 public:
  // Aliased chunks are capped so a slow client holds at most this much of
  // the mmap region in flight per completion round-trip, and so the lease
  // is re-stamped at least once per chunk boundary.  128KB mirrors the
  // chunk size the 2.0 middleware zero-copy serve settled on.
  static constexpr size_t kChunkBytes = 128 * 1024;
  // Final always-copied window (see the header comment).
  static constexpr size_t kTailCopyBytes = 16 * 1024;
  // Aliasing only engages when it aliases something worthwhile: an aliased
  // chunk costs a full async flush round-trip, so a plan that would alias
  // less than this copies the remainder outright instead.  Consequence:
  // bodies below kTailCopyBytes + kMinAliasBytes (32KB) never alias at all
  // (the engine-side gate already floors the alias serve at 16KB; this
  // closes the degenerate 16KB+1 -> 1-byte-aliased-chunk shape).
  static constexpr size_t kMinAliasBytes = 16 * 1024;

  enum class Step {
    kAliasChunk,  // submit [offset, offset+length) aliasing the mmap
    kCopyTail,    // copy [offset, offset+length) == remainder
                  // (copy-then-verify!)
    kAbort,       // epoch moved: remaining bytes unrecoverable
  };

  struct Next {
    Step step;
    size_t offset;
    size_t length;
  };

  explicit IisZeroCopyServePlanner(size_t body_size)
      : body_size_(body_size), offset_(0) {}

  bool done() const { return offset_ >= body_size_; }
  size_t offset() const { return offset_; }
  size_t body_size() const { return body_size_; }

  // Decide the next serve action.  'renew' is the result of
  // MappedSharedString::RenewLeaseStrict() probed immediately before this
  // call on the submit path; 'force_wrap_pressure' is
  // (MappedSharedString::NsUntilForcedWrap() != UINT64_MAX), i.e. any
  // deferred-wrap pressure at all.  Must not be called when done().
  Next PlanNext(LeaseRenewal renew, bool force_wrap_pressure) const {
    const size_t remaining = body_size_ - offset_;
    if (renew == LeaseRenewal::kTorn) {
      return {Step::kAbort, offset_, 0};
    }
    if (renew != LeaseRenewal::kOk || force_wrap_pressure ||
        remaining < kTailCopyBytes + kMinAliasBytes) {
      return {Step::kCopyTail, offset_, remaining};
    }
    // remaining >= kTailCopyBytes + kMinAliasBytes here, so every aliased
    // chunk is at least kMinAliasBytes and the final kTailCopyBytes are
    // left for a later kCopyTail step.
    return {Step::kAliasChunk, offset_,
            std::min(kChunkBytes, remaining - kTailCopyBytes)};
  }

  // Post-exposure verdict for an aliased chunk, probed AFTER its flush
  // completion (copy-then-verify shape: verify after the exposure window
  // closes, never only before it opens).  kTorn means the bytes HTTP.sys
  // just read may have been overwritten mid-read: the serve must abort
  // (the body is never complete at this point -- the tail is always still
  // pending -- so the abort reaches the client as a truncated transfer,
  // never a complete-but-corrupt one).  kOk / kCopyNow / kLeasesOff mean
  // the epoch did not move through the whole in-flight window, so every
  // byte the kernel read was pristine.
  static bool SentBytesTrustworthy(LeaseRenewal renew) {
    return renew != LeaseRenewal::kTorn;
  }

  // Mark [offset(), offset()+bytes) as submitted.
  void Advance(size_t bytes) { offset_ += bytes; }

 private:
  size_t body_size_;
  size_t offset_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_ZEROCOPY_SERVE_H_

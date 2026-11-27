// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Zero-copy serve barrier — pure decision logic.
//
// The nginx module serves cache HITs zero-copy: it aliases the Cyclone mmap
// region (b->memory) or sendfiles from the cache volume fd, then hands the
// buffer to nginx's output filter once.  For a slow client nginx re-drains
// that borrowed region across later event cycles without re-entering the
// module.  Cyclone's lease contract requires an intent-checked
// renew_lease_strict() in the same call stack as each aliased send, plus a
// forced-wrap deadline poll, because the final drain has no later renew.
//
// This header isolates the *decision* — given a lease verdict + forced-wrap
// deadline, keep aliasing, de-alias by copying, or abort — from the nginx
// buffer surgery, so it is unit-testable without nginx headers or a live
// cache.  The module (ngx_pagespeed_module.cc) supplies the surgery.

#ifndef PAGESPEED_SRC_NGINX_ZEROCOPY_BARRIER_H_
#define PAGESPEED_SRC_NGINX_ZEROCOPY_BARRIER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lib/cache/cache.h"  // pagespeed::LeaseRenewal, ReadResult::kNoForcedWrap

namespace pagespeed {
namespace ps_barrier {

// What the barrier decided for one aliased send site.
enum class BarrierAction : std::uint8_t {
  kAlias,    // Lease live, no wrap intent, deadline far: keep aliasing.
  kCopyOut,  // Wrap in flight / leases off / deadline within margin:
             // de-alias by copying the (remaining) bytes into owned memory.
  kAbort,    // Epoch moved (a wrap committed): the region may be torn —
             // fail the serve closed (client gets RST/truncation).
};

// Forced-wrap safety margin (ns).  Unlike mod_pagespeed 1.1 — whose in-process
// filter re-checks the borrow on EVERY writev drain (sub-millisecond cadence),
// so a 500 ms margin dwarfs one burst — the 2.0 nginx module re-checks an
// aliased borrow only on its lease-renewal timer (kLeaseRenewIntervalMs = 3 s).
// A ceiling-forced wrap ignores the lease, so the margin must exceed the gap
// between two strict-renew re-checks (one timer interval) plus one drain burst
// and timer scheduling jitter; otherwise a forced wrap could fall between two
// re-checks unseen and overwrite bytes still on the wire.  4 s = the 3 s recheck
// interval + a 1 s cushion, and stays far below Cyclone's default 60 s
// lease_wrap_ceiling so a normal-speed serve keeps its alias for its whole life.
inline constexpr std::uint64_t kBarrierMarginNs = 4'000ULL * 1000 * 1000;

// Pure barrier decision.  `ns_until_forced_wrap` is ReadResult::kNoForcedWrap
// (UINT64_MAX) when no wrap is deferred.  Total order of the guards matters:
// a kTorn verdict aborts regardless of the deadline; a live-but-near-deadline
// borrow copies out; only a clean far-deadline kOk keeps aliasing.
constexpr BarrierAction ClassifyRenewal(LeaseRenewal renewal,
                                        std::uint64_t ns_until_forced_wrap,
                                        std::uint64_t margin_ns) {
  switch (renewal) {
    case LeaseRenewal::kTorn:
      return BarrierAction::kAbort;
    case LeaseRenewal::kCopyNow:
    case LeaseRenewal::kLeasesOff:
      return BarrierAction::kCopyOut;
    case LeaseRenewal::kOk:
      // A ceiling-forced wrap (which ignores our fresh lease) reachable before
      // the next re-check: copy out now rather than be caught torn later.
      return ns_until_forced_wrap <= margin_ns ? BarrierAction::kCopyOut
                                               : BarrierAction::kAlias;
  }
  return BarrierAction::kCopyOut;  // defensive: unknown verdict copies (safe).
}

// Emit-time copy-gate predicate, extracted pure for unit testing.
// Aliasing a cache HIT is safe ONLY when the buffer handed to the output
// filter is provably the SOLE reference into the borrow: the barrier's drained
// check and tail de-alias operate on that buffer's cursor, and a copy-out
// releases the pin, so any downstream consumer holding its OWN reference into
// the mmap breaks both.  Force the whole-body copy path (never alias, never
// arm the timer) when the emitted buffer is NOT the sole reference:
//   * http_version_gt_11 — HTTP/2+ (and HTTP/3 QUIC) split the buffer into
//     per-frame SHADOW bufs that drain asynchronously as the flow-control
//     window opens WITHOUT advancing our cursor;
//   * has_range        — the range filter emits multipart sub-range bufs that
//     alias the mmap;
//   * is_subrequest    — a subrequest does not own the connection's send loop;
//   * filter_need_in_memory / filter_need_temporary / main_filter_need_in_memory
//     — sub_filter/ssi/charset (re-slice) and gzip/image_filter/xslt (retain a
//     raw next_in pointer across drains) read or keep raw body pointers.
// The nginx module (ps_zerocopy_must_copy) threads the ngx_http_request_t
// fields into these booleans; keeping the logic here makes the h2 copy gate
// testable without nginx headers.
constexpr bool MustCopyForEmit(bool http_version_gt_11, bool has_range,
                               bool is_subrequest, bool filter_need_in_memory,
                               bool filter_need_temporary,
                               bool main_filter_need_in_memory) {
  return http_version_gt_11 || has_range || is_subrequest ||
         filter_need_in_memory || filter_need_temporary ||
         main_filter_need_in_memory;
}

// Mapping of a sendfile buffer's UNSENT span back into the borrowed content()
// view, for the timer-driven tail de-alias: nginx advances b->file_pos as the
// socket accepts bytes, so the unsent tail is [file_pos, file_last) in volume
// coordinates and content() + (file_pos - content_file_offset) in the mmap.
struct SendfileTailSpan {
  bool valid;     // false: the offsets do not map into the borrow (fail closed)
  size_t offset;  // offset of the unsent tail within content()
  size_t length;  // unsent bytes (0 = fully drained, valid)
};

constexpr SendfileTailSpan MapSendfileTail(std::uint64_t file_pos,
                                           std::uint64_t file_last,
                                           std::uint64_t content_file_offset,
                                           size_t content_size) {
  // kNoFileOffset (UINT64_MAX) or a cursor before the content start can never
  // map into the borrow; nor can a backwards or overlong span.
  if (file_last < file_pos || file_pos < content_file_offset ||
      content_file_offset == ReadResult::kNoFileOffset) {
    return {false, 0, 0};
  }
  const std::uint64_t offset = file_pos - content_file_offset;
  const std::uint64_t length = file_last - file_pos;
  if (offset > content_size || length > content_size - offset) {
    return {false, 0, 0};
  }
  return {true, static_cast<size_t>(offset), static_cast<size_t>(length)};
}

// Copy-then-verify core of ps_zerocopy_copy_out / ps_zerocopy_dealias_tail:
// memcpy src->dst, THEN call verify() (the strict lease renew). Returns false
// (discard) on a kTorn verdict: the region may have been overwritten mid-copy.
// verify is a seam: production passes rr->renew_lease_strict, tests inject the
// verdict (and the torn source) deterministically instead of racing for it.
template <typename Verify>
inline bool CopyThenVerify(const void* src, size_t n, void* dst,
                           Verify&& verify) {
  if (n != 0) {
    std::memcpy(dst, src, n);
  }
  return verify() != LeaseRenewal::kTorn;
}

// Repoint a (fully or partially) aliased buffer at request-owned memory after
// a copy-out: the bytes are now pool-owned and mutable, so the buffer must
// read as temporary (NOT memory — memory asserts an immutable alias that
// downstream filters may retain) and every file field must be cleared so the
// write filter can never re-issue a sendfile from the released borrow.
// Templated over the buffer type so the flag/cursor swap is unit-testable
// against a struct mirroring ngx_buf_t's fields, without nginx headers.
template <typename Buf, typename Byte>
inline void RepointToOwned(Buf* b, Byte* owned, size_t len) {
  b->start = owned;
  b->pos = owned;
  b->last = owned + len;
  b->end = owned + len;
  b->memory = 0;
  b->temporary = 1;
  b->in_file = 0;
  b->file = nullptr;
  b->file_pos = 0;
  b->file_last = 0;
}

}  // namespace ps_barrier
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_ZEROCOPY_BARRIER_H_

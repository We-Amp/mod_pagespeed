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

#include "ngx_base_fetch.h"

#include <unistd.h>  //for usleep

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <new>
#include <utility>
#include <variant>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "ngx_event_connection.h"
#include "ngx_list_iterator.h"
#include "ngx_pagespeed.h"  // Must come first, see comments in CollectHeaders.
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

const char kHeadersComplete = 'H';
const char kFlush = 'F';
const char kDone = 'D';

NgxEventConnection* NgxBaseFetch::event_connection = nullptr;
int NgxBaseFetch::active_base_fetches = 0;

namespace {

// Zero-copy aliased serve.  A .pagespeed. cache hit is served by
// handing the Cyclone mmap bytes into r->out by reference (b->memory=1)
// rather than copying.  Correctness comes from a PER-DRAIN barrier
// (ps_zerocopy_barrier in ngx_pagespeed.cc): because that barrier lives in
// the top body filter, nginx runs it before EVERY socket drain of the buf
// (ngx_http_writer -> ngx_http_output_filter -> ps_base_fetch_filter), so
// there is no drain-before-check window a timer would leave open.  This file
// only builds the aliased buf + the pool-scoped pin (PsZeroCopyAlias, in
// ngx_pagespeed.h) and hands it to the request ctx; the barrier owns the
// intent-checked per-send lease revalidation and the copy-out.
//
// At emit, if a ceiling-forced wrap is already reachable within this margin
// the stripe is too hot to alias -- copy the whole body instead.
const uint64_t kEmitMarginNs = 500ULL * 1000 * 1000;  // 500 ms

// Pool cleanup for the aliased pin: releases the Cyclone read-handle
// reference (which unmaps when it was the last).  Pool-scoped so it runs
// even if the NgxBaseFetch was destroyed while a client-paced drain was
// still aliasing the region.
void ps_zerocopy_alias_free(void* data) {
  PsZeroCopyAlias* a = static_cast<PsZeroCopyAlias*>(data);
  delete a->pin;
}

// Pool cleanup for the bounded-copy ring: releases the read-lease
// pin if the ring still holds it -- a client abort mid-stream must not
// leak the lease -- and disarms the refill timer, whose ngx_event_t lives
// in this pool and must not stay on the timer wheel past request free.
void ps_bounded_ring_free(void* data) {
  PsBoundedCopyRing* ring = static_cast<PsBoundedCopyRing*>(data);
  if (ring->refill_ev.timer_set) {
    ngx_del_timer(&ring->refill_ev);
  }
  delete ring->pin;
}

// Buffered segments at least this large are drained by moving the whole
// segment into a heap holder and emitting ONE referenced ngx_buf_t
// (ps_move_segment_to_chain) instead of splitting them into page-sized copied
// buffers.  Below it the copy is kept: a heap holder plus pool cleanup per
// segment costs more than a few page-sized copies, and HTML rewriting drains
// many small tail segments.  Equal to the coalescing threshold so every
// coalesced-to-capacity segment qualifies.
const size_t kMoveSegmentThresholdBytes =
    NgxSegmentBuffer::kCoalesceThresholdBytes;

// Pool cleanup for a segment moved out of NgxSegmentBuffer by
// ps_move_segment_to_chain: frees the heap holder (and with it the bytes the
// emitted ngx_buf_t references) when the request pool is destroyed.
template <class Holder>
void ps_segment_holder_free(void* data) {
  delete static_cast<Holder*>(data);
}

// Emits one referenced (b->memory) buffer over segment's bytes, transferring
// the segment into a heap holder that a pool cleanup frees, so the bytes
// outlive any client-paced async send (the NgxBaseFetch may be destroyed
// first).  For an owned GoogleString segment the transfer is a move; for a
// refcounted SharedString segment it is a reference copy that keeps the
// shared storage alive.  Runs on the nginx thread.
template <class Holder>
ngx_int_t ps_move_segment_to_chain(ngx_pool_t* pool, Holder* segment,
                                   bool send_last_buf, bool send_flush,
                                   ngx_chain_t** link_ptr) {
  *link_ptr = nullptr;
  ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(pool));
  ngx_chain_t* cl = static_cast<ngx_chain_t*>(ngx_alloc_chain_link(pool));
  ngx_pool_cleanup_t* cln = ngx_pool_cleanup_add(pool, 0);
  if (b == nullptr || cl == nullptr || cln == nullptr) {
    return NGX_ERROR;
  }
  // nothrow: an allocation failure must surface as NGX_ERROR, not as an
  // exception unwinding through nginx's C frames.
  Holder* holder = new (std::nothrow) Holder(std::move(*segment));
  if (holder == nullptr) {
    return NGX_ERROR;
  }
  cln->handler = ps_segment_holder_free<Holder>;
  cln->data = holder;
  u_char* base = reinterpret_cast<u_char*>(const_cast<char*>(holder->data()));
  b->start = b->pos = base;
  b->last = b->end = base + holder->size();
  b->memory = 1;  // Read-only for nginx; the pool cleanup frees it.
  if (send_flush) {
    b->flush = 1;
  }
  if (send_last_buf) {
    b->last_buf = 1;
  }
  cl->buf = b;
  cl->next = nullptr;
  *link_ptr = cl;
  return NGX_OK;
}

}  // namespace

NgxBaseFetch::NgxBaseFetch(StringPiece url, ngx_http_request_t* r,
                           NgxServerContext* server_context,
                           const RequestContextPtr& request_ctx,
                           PreserveCachingHeaders preserve_caching_headers,
                           NgxBaseFetchType base_fetch_type,
                           const RewriteOptions* options)
    : AsyncFetch(request_ctx),
      url_(url.data(), url.size()),
      request_(r),
      server_context_(server_context),
      options_(options),
      need_flush_(false),
      done_called_(false),
      last_buf_sent_(false),
      references_(2),
      base_fetch_type_(base_fetch_type),
      preserve_caching_headers_(preserve_caching_headers),
      detached_(false),
      suppress_(false) {
  if (pthread_mutex_init(&mutex_, nullptr)) CHECK(0);
  __sync_add_and_fetch(&NgxBaseFetch::active_base_fetches, 1);
}

NgxBaseFetch::~NgxBaseFetch() {
  pthread_mutex_destroy(&mutex_);
  __sync_add_and_fetch(&NgxBaseFetch::active_base_fetches, -1);
}

bool NgxBaseFetch::Initialize(ngx_cycle_t* cycle) {
  CHECK(event_connection == nullptr) << "event connection already set";
  event_connection = new NgxEventConnection(ReadCallback);
  return event_connection->Init(cycle);
}

void NgxBaseFetch::Terminate() {
  if (event_connection != nullptr) {
    GoogleMessageHandler handler;
    PosixTimer timer;
    int64 timeout_us = Timer::kSecondUs * 30;
    int64 end_us = timer.NowUs() + timeout_us;
    static unsigned int sleep_microseconds = 100;

    handler.Message(
        kInfo, "NgxBaseFetch::Terminate rounding up %d active base fetches.",
        NgxBaseFetch::active_base_fetches);

    // Try to continue processing and get the active base fetch count to 0
    // until the timeout expires.
    // TODO(oschaaf): This needs more work.
    while (NgxBaseFetch::active_base_fetches > 0 && end_us > timer.NowUs()) {
      event_connection->Drain();
      usleep(sleep_microseconds);
    }

    if (NgxBaseFetch::active_base_fetches != 0) {
      handler.Message(
          kWarning,
          "NgxBaseFetch::Terminate timed out with %d active base fetches.",
          NgxBaseFetch::active_base_fetches);
    }

    // Close down the named pipe.
    event_connection->Shutdown();
    delete event_connection;
    event_connection = nullptr;
  }
}

const char* BaseFetchTypeToCStr(NgxBaseFetchType type) {
  switch (type) {
    case kPageSpeedResource:
      return "ps resource";
    case kHtmlTransform:
      return "html transform";
    case kAdminPage:
      return "admin page";
    case kIproLookup:
      return "ipro lookup";
    case kPageSpeedProxy:
      return "pagespeed proxy";
  }
  CHECK(false);
  return "can't get here";
}

// TODO(oschaaf): replace the ngx_log_error with VLOGS or pass in a
// MessageHandler and use that.
void NgxBaseFetch::ReadCallback(const ps_event_data& data) {
  NgxBaseFetch* base_fetch = reinterpret_cast<NgxBaseFetch*>(data.sender);
  ngx_http_request_t* r = base_fetch->request();
  bool detached = base_fetch->detached();
#if (NGX_DEBUG)  // `type` is unused if NGX_DEBUG isn't set, needed for -Werror.
  const char* type = BaseFetchTypeToCStr(base_fetch->base_fetch_type_);
#endif
  int refcount = base_fetch->DecrementRefCount();

#if (NGX_DEBUG)
  ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                "pagespeed [%p] event: %c. bf:%p (%s) - refcnt:%d - det: %c", r,
                data.type, base_fetch, type, refcount, detached ? 'Y' : 'N');
#endif

  // If we ended up destructing the base fetch, or the request context is
  // detached, skip this event.
  if (refcount == 0 || detached) {
    return;
  }

  ps_request_ctx_t* ctx = ps_get_request_context(r);

  // If our request context was zeroed, skip this event.
  // See https://github.com/apache/incubator-pagespeed-ngx/issues/1081
  if (ctx == nullptr) {
    // Should not happen normally, when it does this message will cause our
    // system tests to fail.
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                  "pagespeed [%p] skipping event: request context gone", r);
    return;
  }

  // Normally we expect the sender to equal the active NgxBaseFetch instance.
  DCHECK(data.sender == ctx->base_fetch);

  // If someone changed our request context or NgxBaseFetch, skip processing.
  if (data.sender != ctx->base_fetch) {
    ngx_log_error(
        NGX_LOG_WARN, ngx_cycle->log, 0,
        "pagespeed [%p] skipping event: event originating from disassociated"
        " NgxBaseFetch instance.",
        r);
    return;
  }

  int rc;
  bool run_posted = true;
  // If we are unlucky enough to have our connection finalized mid-ipro-lookup,
  // we must enter a different flow. Also see ps_in_place_check_header_filter().
  if ((ctx->base_fetch->base_fetch_type_ != kIproLookup) &&
      r->connection->error) {
    ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                  "pagespeed [%p] request already finalized %d", r, r->count);
    rc = NGX_ERROR;
    run_posted = false;
  } else {
    rc = ps_base_fetch::ps_base_fetch_handler(r);
  }

#if (NGX_DEBUG)
  ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                "pagespeed [%p] ps_base_fetch_handler() returned %d for %c", r,
                rc, data.type);
#endif

  ngx_connection_t* c = r->connection;
  ngx_http_finalize_request(r, rc);

  if (run_posted) {
    // See http://forum.nginx.org/read.php?2,253006,253061
    ngx_http_run_posted_requests(c);
  }
}

void NgxBaseFetch::Lock() { pthread_mutex_lock(&mutex_); }

void NgxBaseFetch::Unlock() { pthread_mutex_unlock(&mutex_); }

bool NgxBaseFetch::HandleWrite(const StringPiece& sp, MessageHandler* handler) {
  Lock();
  buffer_.Append(sp);
  Unlock();
  return true;
}

bool NgxBaseFetch::HandleWriteShared(const StringPiece& content,
                                     const SharedString& storage,
                                     MessageHandler* handler) {
  // Below the take-ownership drain threshold the bytes get copied into
  // page-sized buffers at drain time anyway, so the refcount bookkeeping
  // would only add overhead.
  if (content.size() < kMoveSegmentThresholdBytes) {
    return HandleWrite(content, handler);
  }
  const StringPiece all = storage.Value();
  // std::less gives a total pointer order, so the containment check is
  // well-defined even when 'content' does not point into 'all' at all --
  // exactly the case it guards against.
  const std::less<const char*> before;
  if (before(content.data(), all.data()) ||
      before(all.data() + all.size(), content.data() + content.size())) {
    DCHECK(false) << "WriteShared content must alias its storage";
    return HandleWrite(content, handler);  // Release-safe: serve a copy.
  }
  // Trim a refcounted view down to the content window.  RemovePrefix and
  // RemoveSuffix adjust only this view's bounds, never the shared bytes.
  const int prefix = static_cast<int>(content.data() - all.data());
  const int suffix =
      static_cast<int>(all.size()) - prefix - static_cast<int>(content.size());
  SharedString view(storage);
  view.RemovePrefix(prefix);
  view.RemoveSuffix(suffix);
  Lock();
  buffer_.AppendShared(view);
  Unlock();
  return true;
}

bool NgxBaseFetch::WriteMapped(const StringPiece& mmap_sp,
                               const MappedSharedString& keepalive,
                               MessageHandler* handler) {
  Lock();
  // Record the mmap region as a BORROWED blob: alias, no copy, no append.
  borrowed_data_ = mmap_sp.data();
  borrowed_size_ = mmap_sp.size();
  has_borrowed_ = true;
  pinned_pending_ = keepalive;  // refcount bump on the read handle.
  Unlock();
  return true;
}

// should only be called in nginx thread
ngx_int_t NgxBaseFetch::CopyBufferToNginx(ngx_chain_t** link_ptr) {
  CHECK(!(done_called_ && last_buf_sent_))
      << "CopyBufferToNginx() was called after the last buffer was sent";

  // there is no buffer to send
  if (!done_called_ && buffer_.empty() && !has_borrowed_) {
    *link_ptr = nullptr;
    return NGX_AGAIN;
  }

  *link_ptr = nullptr;
  int rc = NGX_OK;
  if (has_borrowed_) {
    // Zero-copy ALIASED serve (CycloneZeroCopyServe).  Runs on the nginx
    // thread with mutex_ held.  Emit-time guard: if a ceiling-forced wrap
    // is reachable within kEmitMarginNs, the stripe is contended and the
    // brief aliased exposure would be unsafe -- copy the whole body now.
    // Otherwise alias and arm the single-shot slow-client copy-out.
    if (!buffer_.empty()) {
      // Never expected (a mapped serve is a single WriteMapped); fall back
      // to the copying path release-safely rather than drop bytes.
      has_borrowed_ = false;
      pinned_pending_ = MappedSharedString();
    } else {
      // Emit-time aliasing decision (intent-checked).  RenewLeaseStrict()
      // stamps a fresh lease AND Dekker-revalidates the borrow (wrap_intent
      // loaded before epoch), the same protocol the initial borrow uses --
      // unlike the epoch-only renew it is sound for the aliased path, whose
      // send is an unobservable socket write, and it distinguishes leases-off
      // from a torn borrow so a valid serve is never RST just because leases
      // are disabled.
      const LeaseRenewal r0 = pinned_pending_.RenewLeaseStrict();
      const bool force_imminent =
          pinned_pending_.NsUntilForcedWrap() <= kEmitMarginNs;
      // Downstream body filters that read, re-slice, or RETAIN a raw pointer
      // into the body keep aliasing our mmap past the buf the per-drain
      // barrier tracks, so aliasing is unsafe when any is active; copy instead:
      //   * sub_filter/ssi/addition/charset set filter_need_in_memory
      //     (charset also filter_need_temporary);
      //   * gzip/gunzip/image_filter/xslt set main_filter_need_in_memory
      //     (gzip drives zlib from a raw mmap next_in a slow client suspends);
      //   * a Range request drives the range filter (multipart sub-range bufs);
      //   * HTTP/2 (ngx_http_v2_send_chain) splits our buf into per-frame
      //     SHADOW bufs aliasing our mmap, sent async as the flow-control
      //     window opens and setting none of the in-memory flags.  HTTP/3
      //     QUIC framing has the same shape, and its safety would rest on
      //     build-specific QUIC-copies-synchronously invariants, so we force a
      //     copy for ANY non-HTTP/1.x version (> NGX_HTTP_VERSION_11 catches
      //     both h2 and h3) rather than depend on them.
      // A subrequest does not own the connection body-send loop, so its drains
      // do not route through our top-body-filter barrier -- copy.
      const bool downstream_reads_body = request_->filter_need_in_memory ||
                                         request_->filter_need_temporary ||
                                         request_->main_filter_need_in_memory;
      ps_request_ctx_t* rctx = ps_get_request_context(request_);
      // Alias ONLY when the borrow is provably safe right now (kOk) AND we can
      // register the per-drain barrier on the request ctx; any other lease
      // state (kCopyNow wrap-in-flight, kTorn epoch moved, kLeasesOff no
      // protection) or an unaliasable serve forces the copy path.
      const bool must_copy = (r0 != LeaseRenewal::kOk) || force_imminent ||
                             downstream_reads_body ||
                             request_->headers_in.range != nullptr ||
                             request_->http_version > NGX_HTTP_VERSION_11 ||
                             request_ != request_->main || rctx == nullptr;
      Statistics* stats = server_context_->statistics();
      Variable* aliased_stat =
          stats != nullptr ? stats->FindVariable("zerocopy_serve_aliased")
                           : nullptr;
      Variable* copied_stat =
          stats != nullptr ? stats->FindVariable("zerocopy_serve_copied_out")
                           : nullptr;
      Variable* renew_fail_stat =
          stats != nullptr
              ? stats->FindVariable("zerocopy_serve_renew_fail_reset")
              : nullptr;
      // Bounded-copy ring : when the ONLY must_copy cause is the
      // protocol (h2/h3 framing -- lease state permits, no in-memory
      // filter, no Range, main request, ctx present), the whole-body copy's
      // O(body) per-stream residency is bounded to O(ring) instead:
      // kRingSlotBytes windows are copied out of the pinned region on
      // demand as the flow-control window drains, refilled by
      // ps_base_fetch_filter on the same top-filter re-entry clock the
      // per-drain barrier uses.  Filters that set filter_need_in_memory /
      // temporary retain raw pointers across invocations, so buffer
      // recycling under them corrupts -- they keep the whole-body copy.
      // done_called_ is required so the final window can carry last_buf (a
      // mapped serve is a single WriteMapped; if the Done event has not
      // been consumed yet, keep today's whole-body copy rather than race
      // the trailing empty last_buf buffer).
      const bool ring_eligible =
          must_copy && request_->http_version > NGX_HTTP_VERSION_11 &&
          r0 == LeaseRenewal::kOk && !force_imminent &&
          !downstream_reads_body && request_->headers_in.range == nullptr &&
          request_ == request_->main && rctx != nullptr && done_called_ &&
          borrowed_size_ > kRingMinBodyBytes;
      if (ring_eligible) {
        PsBoundedCopyRing* ring = static_cast<PsBoundedCopyRing*>(
            ngx_pcalloc(request_->pool, sizeof(PsBoundedCopyRing)));
        ngx_pool_cleanup_t* cln = ngx_pool_cleanup_add(request_->pool, 0);
        if (ring == nullptr || cln == nullptr) {
          return NGX_ERROR;
        }
        for (size_t i = 0; i < kRingSlotCount; i++) {
          u_char* window =
              static_cast<u_char*>(ngx_pnalloc(request_->pool, kRingSlotBytes));
          ngx_buf_t* slot =
              static_cast<ngx_buf_t*>(ngx_calloc_buf(request_->pool));
          if (window == nullptr || slot == nullptr) {
            return NGX_ERROR;
          }
          // Fixed backing store; pos/last are set per window by
          // ps_bounded_ring_emit_next.  pos == last is the byte-granular
          // "accepted by the socket" signal that makes a slot reusable.
          slot->start = window;
          slot->end = window + kRingSlotBytes;
          slot->pos = slot->last = window;
          slot->temporary = 1;
          ring->slot[i] = slot;
        }
        ring->src = borrowed_data_;
        ring->size = borrowed_size_;
        // The pool cleanup is the ownership root for the pin, exactly like
        // PsZeroCopyAlias: a client abort mid-stream must release the read
        // lease even though this NgxBaseFetch is long gone by then.
        ring->pin = new MappedSharedString(std::move(pinned_pending_));
        ring->refills_stat =
            stats != nullptr
                ? stats->FindVariable("zerocopy_serve_ring_refills")
                : nullptr;
        ring->copied_out_stat = copied_stat;
        ring->renew_fail_stat = renew_fail_stat;
        cln->handler = ps_bounded_ring_free;
        cln->data = ring;
        rctx->bounded_ring = ring;
        // Arm the refill clock (see PsBoundedCopyRing::refill_ev).  The
        // event struct is pool memory zeroed by pcalloc; the cleanup above
        // disarms it on any teardown path.
        ring->refill_ev.handler = ps_base_fetch::ps_bounded_ring_refill_event;
        ring->refill_ev.data = request_;
        ring->refill_ev.log = request_->connection->log;
        ngx_add_timer(&ring->refill_ev, 1);
        pinned_pending_ = MappedSharedString();
        has_borrowed_ = false;
        // Initial fill: both windows, emitted as one chain.  borrowed_size_
        // exceeds kRingMinBodyBytes, so neither initial window can be the
        // final one unless a degrade collapsed the tail (in which case the
        // emitted tail buf carried last_buf itself).
        ngx_chain_t** tail = link_ptr;
        for (size_t i = 0; i < kRingSlotCount && !ring->done; i++) {
          ngx_chain_t* cl = nullptr;
          if (ps_base_fetch::ps_bounded_ring_emit_next(request_, ring, &cl) !=
              NGX_OK) {
            return NGX_ERROR;  // torn window: fail the serve closed.
          }
          *tail = cl;
          tail = &cl->next;
        }
        if (need_flush_ && *link_ptr != nullptr) {
          ngx_chain_t* last_link = *link_ptr;
          while (last_link->next != nullptr) {
            last_link = last_link->next;
          }
          last_link->buf->flush = 1;
          need_flush_ = false;
        }
        // The un-emitted remainder is owned by rctx->bounded_ring + the
        // pool cleanup (the same shape as the aliased buf outliving this
        // object), so this fetch is complete: done_called_ held above, and
        // the final window carries last_buf.
        last_buf_sent_ = true;
        return NGX_OK;
      }
      ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(request_->pool));
      if (b == nullptr) {
        return NGX_ERROR;
      }
      if (must_copy) {
        // Copy the whole body, then copy-then-verify: a forced (ceiling) wrap
        // ignores our fresh lease and could race the pwrite during a large
        // memcpy, so re-check the borrow AFTER the copy.  RST ONLY on a
        // genuine epoch move (kTorn); kOk/kCopyNow (region intact) and
        // kLeasesOff (legacy, no protection) serve the copy -- leases-off must
        // never turn every serve into a reset.
        u_char* owned =
            static_cast<u_char*>(ngx_pnalloc(request_->pool, borrowed_size_));
        if (owned == nullptr) {
          return NGX_ERROR;
        }
        ngx_memcpy(owned, borrowed_data_, borrowed_size_);
        const LeaseRenewal rv = pinned_pending_.RenewLeaseStrict();
        pinned_pending_ = MappedSharedString();  // copied: drop the pin.
        if (rv == LeaseRenewal::kTorn) {
          if (renew_fail_stat != nullptr) {
            renew_fail_stat->Add(1);
          }
          return NGX_ERROR;  // region overwritten mid/pre-copy: torn body.
        }
        b->start = b->pos = owned;
        b->last = b->end = owned + borrowed_size_;
        b->memory = 0;
        b->temporary = 1;
        if (copied_stat != nullptr) {
          copied_stat->Add(1);
        }
      } else {
        // Alias the mmap region (read-only): nginx must not mutate/free.
        u_char* base =
            reinterpret_cast<u_char*>(const_cast<char*>(borrowed_data_));
        b->start = b->pos = base;
        b->last = b->end = base + borrowed_size_;
        b->memory = 1;
        b->temporary = 0;
      }
      if (need_flush_) {
        b->flush = 1;
        need_flush_ = false;
      }
      if (done_called_) {
        b->last_buf = 1;
      }
      ngx_chain_t* cl =
          static_cast<ngx_chain_t*>(ngx_alloc_chain_link(request_->pool));
      if (cl == nullptr) {
        return NGX_ERROR;
      }
      cl->buf = b;
      cl->next = nullptr;
      *link_ptr = cl;
      if (!must_copy) {
        // Aliased: hand the pin to a pool-scoped PsZeroCopyAlias and register
        // it on the request ctx.  ps_base_fetch_filter (the top body filter)
        // runs the intent-checked per-drain barrier over it before EVERY
        // socket drain -- no timer, no drain-before-check window.  A pool
        // cleanup releases the pin even if this NgxBaseFetch is destroyed
        // while a client-paced drain is still aliasing the region.
        PsZeroCopyAlias* a = static_cast<PsZeroCopyAlias*>(
            ngx_pcalloc(request_->pool, sizeof(PsZeroCopyAlias)));
        ngx_pool_cleanup_t* cln = ngx_pool_cleanup_add(request_->pool, 0);
        if (a == nullptr || cln == nullptr) {
          return NGX_ERROR;
        }
        a->buf = b;
        a->pin = new MappedSharedString(std::move(pinned_pending_));
        a->copied_out_stat = copied_stat;
        a->renew_fail_stat = renew_fail_stat;
        a->done = false;
        cln->handler = ps_zerocopy_alias_free;
        cln->data = a;
        rctx->zerocopy_alias = a;  // rctx != nullptr (in must_copy otherwise).
        if (aliased_stat != nullptr) {
          aliased_stat->Add(1);
        }
      }
      has_borrowed_ = false;
      if (done_called_) {
        last_buf_sent_ = true;
        return NGX_OK;
      }
      return NGX_AGAIN;
    }
  }
  if (buffer_.empty()) {
    // done_called_ with no body bytes buffered: emit the single empty sync
    // buffer whose only purpose is to carry last_buf (and flush).
    rc = string_piece_to_buffer_chain(request_->pool, "", link_ptr,
                                      done_called_ /* send_last_buf */,
                                      need_flush_);
  } else {
    // Convert each buffered segment to a buffer chain, linking the chains
    // together. Only the very last buffer may carry flush/last_buf, matching
    // the single-string behavior this replaces. Segments are never empty, so
    // no spurious sync buffers are generated here. Segments of at least
    // kMoveSegmentThresholdBytes are not copied: the drain takes ownership
    // and emits one referenced buffer per segment (ps_move_segment_to_chain).
    ngx_chain_t* tail_link = nullptr;
    std::deque<NgxSegmentBuffer::Segment> segments = buffer_.TakeSegments();
    for (size_t i = 0; i < segments.size(); i++) {
      bool last_segment = (i + 1 == segments.size());
      bool send_last_buf = done_called_ && last_segment;
      bool send_flush = need_flush_ && last_segment;
      ngx_chain_t* segment_chain = nullptr;
      GoogleString* owned = std::get_if<GoogleString>(&segments[i]);
      if (owned == nullptr) {
        // A refcounted shared-storage segment (HandleWriteShared) is emitted
        // by reference -- UNLESS a downstream filter will mutate the buf in
        // place: the charset filter's single-byte recode rewrites
        // b->pos..b->last directly.  It sets r->filter_need_temporary, but
        // the only filter that enforces that flag (the copy filter) sits
        // ABOVE this module's body-filter injection point, so it never
        // protects our bufs.  A recode of a shared reference would corrupt
        // the cached value for every request sharing it, so emit a private
        // copy instead.  This runs on the nginx thread after the header
        // filters (which set the flag), unlike the PSOL-side write.
        SharedString* shared = &std::get<SharedString>(segments[i]);
        if (request_->filter_need_temporary) {
          rc = string_piece_to_buffer_chain(request_->pool, shared->Value(),
                                            &segment_chain, send_last_buf,
                                            send_flush);
        } else {
          rc = ps_move_segment_to_chain(request_->pool, shared, send_last_buf,
                                        send_flush, &segment_chain);
        }
      } else if (owned->size() >= kMoveSegmentThresholdBytes) {
        rc = ps_move_segment_to_chain(request_->pool, owned, send_last_buf,
                                      send_flush, &segment_chain);
      } else {
        rc = string_piece_to_buffer_chain(
            request_->pool, *owned, &segment_chain, send_last_buf, send_flush);
      }
      if (rc != NGX_OK) {
        break;
      }
      if (*link_ptr == nullptr) {
        *link_ptr = segment_chain;
      } else {
        tail_link->next = segment_chain;
      }
      tail_link = segment_chain;
      while (tail_link->next != nullptr) {
        tail_link = tail_link->next;
      }
    }
  }
  need_flush_ = false;

  if (rc != NGX_OK) {
    return rc;
  }

  if (done_called_) {
    last_buf_sent_ = true;
    return NGX_OK;
  }

  return NGX_AGAIN;
}

// There may also be a race condition if this is called between the last Write()
// and Done() such that we're sending an empty buffer with last_buf set, which I
// think nginx will reject.
ngx_int_t NgxBaseFetch::CollectAccumulatedWrites(ngx_chain_t** link_ptr) {
  ngx_int_t rc;
  Lock();
  rc = CopyBufferToNginx(link_ptr);
  Unlock();
  return rc;
}

ngx_int_t NgxBaseFetch::CollectHeaders(ngx_http_headers_out_t* headers_out) {
  // nginx defines _FILE_OFFSET_BITS to 64, which changes the size of off_t.
  // If a standard header is accidentally included before the nginx header,
  // on a 32-bit system off_t will be 4 bytes and we don't assign all the
  // bits of content_length_n. Sanity check that did not happen.
  // This could use static_assert, but this file is not built with --std=c++11.
  bool sanity_check_off_t[sizeof(off_t) == 8 ? 1 : -1] __attribute__((unused));

  ResponseHeaders* pagespeed_headers = response_headers();

  if (content_length_known()) {
    headers_out->content_length = nullptr;
    headers_out->content_length_n = content_length();
  }

  // Add a weak ETag for .pagespeed. resource responses if PSOL didn't set one.
  // Apache gets ETags from its built-in FileETag directive; nginx needs this
  // explicitly for cache-extended resources.
  if (base_fetch_type_ == kPageSpeedResource &&
      !pagespeed_headers->Has(HttpAttributes::kEtag)) {
    pagespeed_headers->Add(HttpAttributes::kEtag, "W/\"0\"");
    pagespeed_headers->ComputeCaching();
  }

  return copy_response_headers_to_ngx(request_, *pagespeed_headers,
                                      preserve_caching_headers_);
}

void NgxBaseFetch::RequestCollection(char type) {
  if (suppress_) {
    return;
  }

  // We must optimistically increment the refcount, and decrement it
  // when we conclude we failed. If we only increment on a successfull write,
  // there's a small chance that between writing and adding to the refcount
  // both pagespeed and nginx will release their refcount -- destructing
  // this NgxBaseFetch instance.
  IncrementRefCount();
  if (!event_connection->WriteEvent(type, this)) {
    DecrementRefCount();
  }
}

void NgxBaseFetch::HandleHeadersComplete() {
  int status_code = response_headers()->status_code();
  bool status_ok = (status_code != 0) && (status_code < 400);

  if ((base_fetch_type_ != kIproLookup) || status_ok) {
    // If this is a 404 response we need to count it in the stats.
    if (response_headers()->status_code() == HttpStatus::kNotFound) {
      server_context_->rewrite_stats()->resource_404_count()->Add(1);
    }
  }

  RequestCollection(kHeadersComplete);  // Headers available.

  // For the IPRO lookup, supress notification of the nginx side here.
  // If we send both the headerscomplete event and the one from done, nasty
  // stuff will happen if we loose the race with with the nginx side destructing
  // this base fetch instance.
  if (base_fetch_type_ == kIproLookup && !status_ok) {
    suppress_ = true;
  }
}

bool NgxBaseFetch::HandleFlush(MessageHandler* handler) {
  Lock();
  need_flush_ = true;
  Unlock();
  RequestCollection(kFlush);  // A new part of the response body is available
  return true;
}

int NgxBaseFetch::DecrementRefCount() {
  return DecrefAndDeleteIfUnreferenced();
}

int NgxBaseFetch::IncrementRefCount() {
  return __sync_add_and_fetch(&references_, 1);
}

int NgxBaseFetch::DecrefAndDeleteIfUnreferenced() {
  // Creates a full memory barrier.
  int r = __sync_add_and_fetch(&references_, -1);
  if (r == 0) {
    delete this;
  }
  return r;
}

void NgxBaseFetch::HandleDone(bool success) {
  // TODO(jefftk): it's possible that instead of locking here we can just modify
  // CopyBufferToNginx to only read done_called_ once.
  CHECK(!done_called_) << "Done already called!";
  Lock();
  done_called_ = true;
  Unlock();
  RequestCollection(kDone);
  DecrefAndDeleteIfUnreferenced();
}

bool NgxBaseFetch::IsCachedResultValid(const ResponseHeaders& headers) {
  return OptionsAwareHTTPCacheCallback::IsCacheValid(
      url_, *options_, request_context(), headers);
}

}  // namespace net_instaweb

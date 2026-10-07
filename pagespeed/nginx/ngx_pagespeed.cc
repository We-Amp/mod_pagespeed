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

/*
 * Usage:
 *   server {
 *     pagespeed    on|off;
 *   }
 */

#include "ngx_pagespeed.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "absl/strings/str_format.h"
#include "log_message_handler.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/cache_url_async_fetcher.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/rewriter/public/experiment_matcher.h"
#include "net/instaweb/rewriter/public/experiment_util.h"
#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/resource_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/fallback_property_page.h"
#include "ngx_admin_path_match.h"
#include "ngx_base_fetch.h"
#include "ngx_caching_headers.h"
#include "ngx_daemon_record_completion.h"
#include "ngx_daemon_serve_emit.h"
#include "ngx_daemon_serve_notify.h"
#include "ngx_etag_match.h"
#include "ngx_gzip_setter.h"
#include "ngx_list_iterator.h"
#include "ngx_message_handler.h"
#include "ngx_rewrite_driver_factory.h"
#include "ngx_rewrite_options.h"
#include "ngx_server_context.h"
#include "ngx_static_asset_path.h"
#include "ngx_webbotauth_handler.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/thread/pthread_shared_mem.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "pagespeed/kernel/util/statistics_logger.h"
#include "pagespeed/kernel/webbotauth/key_directory_warmer.h"
#include "pagespeed/kernel/webbotauth/webbotauth_counter_store.h"
#include "pagespeed/system/admin_site.h"
#include "pagespeed/system/daemon_ipro_recorder.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/in_place_resource_recorder.h"  // the gate returns it
#include "pagespeed/system/ipro_record_gate.h"
#include "pagespeed/system/ipro_recorder.h"
#include "pagespeed/system/serve_host_names.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "pagespeed/system/system_thread_system.h"

extern ngx_module_t ngx_pagespeed;

// Unused flag, see
// http://lxr.evanmiller.org/http/source/http/ngx_http_request.h#L130
#define NGX_HTTP_PAGESPEED_BUFFERED 0x08
#define POST_BUF_READ_SIZE 65536
#define ADMIN_MAX_POST_SIZE 8192

// Needed for SystemRewriteDriverFactory to use shared memory.
#define PAGESPEED_SUPPORT_POSIX_SHARED_MEM
#define NGINX_1_13_4 1013004

net_instaweb::NgxRewriteDriverFactory* active_driver_factory = nullptr;

namespace net_instaweb {

// The process context takes care of proactively initialising
// a few libraries for us, some of which are not thread-safe
// when they are initialized lazily.
ProcessContext* process_context = new ProcessContext();
bool process_context_cleanup_hooked = false;

// This worker's pool and sequence for daemon record completions: the one
// call a recorder finishes with -- DoneAndSetHeaders -- blocks on the
// daemon's volume, so it is carried here and run off the event thread.
// The pool is this port's OWN, one worker, created in ps_init_child_process
// only for a serving process with at least one server whose startup verdict
// lets it record: a commit stalled in the daemon must never starve an image
// rewrite on the factory's pools, and the master and nginx's cache manager
// and loader -- which serve nothing -- get no pool at all.  ps_exit_child_process
// shuts it down and deletes it after the factory's ShutDown() and before
// the volume-close loop; the sequence is deleted with the pool, so there is
// deliberately no FreeSequence anywhere.  nullptr where no pool exists,
// which the record path reads as "no queue": such a recording is finished
// inline as incomplete.
QueuedWorkerPool* g_daemon_record_pool = nullptr;
QueuedWorkerPool::Sequence* g_daemon_record_sequence = nullptr;

StringPiece str_to_string_piece(ngx_str_t s) {
  return StringPiece(reinterpret_cast<char*>(s.data), s.len);
}

// Nginx uses memory pools, like Apache, allocating strings a request needs from
// the pool and then throwing it all away when the request finishes.  Use this
// when you need to pass a short string to nginx and want it to take ownership
// of the string.
char* string_piece_to_pool_string(ngx_pool_t* pool, StringPiece sp) {
  // Need space for the final null.
  ngx_uint_t buffer_size = sp.size() + 1;
  char* s = static_cast<char*>(ngx_palloc(pool, buffer_size));
  if (s == nullptr) {
    LOG(ERROR) << "string_piece_to_pool_string: ngx_palloc() returned NULL";
    DCHECK(false);
    return nullptr;
  }
  sp.copy(s, buffer_size /* max to copy */);
  s[buffer_size - 1] = '\0';  // Null terminate it.
  return s;
}

// When passing the body of http responses between filters Nginx uses a linked
// list of buffers ("buffer chain"), again like Apache.  This constructs one of
// those lists from a StringPiece.  This is what you use when you need to pass a
// (potentially) longer string to nginx and want it to take ownership.
ngx_int_t string_piece_to_buffer_chain(ngx_pool_t* pool, StringPiece sp,
                                       ngx_chain_t** link_ptr,
                                       bool send_last_buf, bool send_flush) {
  // Below, *link_ptr will be NULL if we're starting the chain, and the head
  // chain link.
  *link_ptr = nullptr;

  // If non-null, the current last link in the chain.
  ngx_chain_t* tail_link = nullptr;

  // How far into sp we're currently working on.
  ngx_uint_t offset;

  // Other modules seem to default to ngx_pagesize.
  ngx_uint_t max_buffer_size = ngx_pagesize;
  for (offset = 0;
       offset < sp.size() ||
       // If we need to send the last buffer bit and there's no data, we
       // should send a single empty buffer.  Otherwise we shouldn't
       // generate empty buffers.
       (offset == 0 && sp.size() == 0);
       offset += max_buffer_size) {
    // Prepare a new nginx buffer to put our buffered writes into.
    ngx_buf_t* b = static_cast<ngx_buf_t*>(ngx_calloc_buf(pool));
    if (b == nullptr) {
      return NGX_ERROR;
    }

    if (sp.size() == 0) {
      CHECK(offset == 0);  // NOLINT
      b->pos = b->start = b->end = b->last = nullptr;
      // The purpose of this buffer is just to pass along last_buf.
      b->sync = 1;
    } else {
      CHECK(sp.size() > offset);
      ngx_uint_t b_size = sp.size() - offset;
      if (b_size > max_buffer_size) {
        b_size = max_buffer_size;
      }

      b->start = b->pos = static_cast<u_char*>(ngx_palloc(pool, b_size));
      if (b->pos == nullptr) {
        return NGX_ERROR;
      }

      // Copy our writes over.  We're copying from sp[offset] up to
      // sp[offset + b_size] into b which has size b_size.
      sp.copy(reinterpret_cast<char*>(b->pos), b_size, offset);
      b->last = b->end = b->pos + b_size;

      b->temporary = 1;  // Identify this buffer as in-memory and mutable.
    }

    // Prepare a chain link.
    ngx_chain_t* cl = static_cast<ngx_chain_t*>(ngx_alloc_chain_link(pool));
    if (cl == nullptr) {
      return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = nullptr;

    if (*link_ptr == nullptr) {
      // This is the first link in the returned chain.
      *link_ptr = cl;
    } else {
      // Link us into the chain.
      CHECK(tail_link != nullptr);
      tail_link->next = cl;
    }

    tail_link = cl;
  }

  CHECK(tail_link != nullptr);
  if (send_flush) {
    tail_link->buf->flush = true;
  }
  if (send_last_buf) {
    tail_link->buf->last_buf = true;
  }

  return NGX_OK;
}

// Get the context for this request.  ps_connection_read_handler should already
// have been called to create it.
ps_request_ctx_t* ps_get_request_context(ngx_http_request_t* r) {
  return static_cast<ps_request_ctx_t*>(
      ngx_http_get_module_ctx(r, ngx_pagespeed));
}

// Tell nginx whether we have network activity we're waiting for so that it sets
// a write handler.  See src/http/ngx_http_request.c:2083.
void ps_set_buffered(ngx_http_request_t* r, bool on) {
  if (on) {
    r->buffered |= NGX_HTTP_PAGESPEED_BUFFERED;
  } else {
    r->buffered &= ~NGX_HTTP_PAGESPEED_BUFFERED;
  }
}

namespace {

void ps_release_base_fetch(ps_request_ctx_t* ctx);

}  // namespace

namespace ps_base_fetch {

ngx_http_output_header_filter_pt ngx_http_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_next_body_filter;

// Forced-wrap safety margin.  Keep aliasing only while a ceiling-
// forced wrap is at least this far off (>> one drain's writev burst); below
// it, de-alias the tail proactively.  Far under the lease ceiling, so a
// normal-speed serve stays aliased its whole life.
const uint64_t kBarrierMarginNs = 1000ULL * 1000 * 1000;  // 1 s

// Per-drain zero-copy barrier.  Runs in the write call stack (this is
// the top body filter, re-entered by ngx_http_writer -> ngx_http_output_filter
// before every socket drain) BEFORE the aliased buf is handed to the write
// filter, so an aliased writev is always immediately preceded (same stack, µs)
// by an intent-checked lease revalidation.  Returns NGX_OK to proceed (the buf
// may have been de-aliased in place) or NGX_ERROR to fail the serve closed (a
// wrap committed -> the unsent tail is torn and Content-Length is already on
// the wire).
ngx_int_t ps_zerocopy_barrier(ngx_http_request_t* r, PsZeroCopyAlias* a) {
  ngx_buf_t* b = a->buf;
  if (a->done || b->memory == 0) {
    return NGX_OK;  // already de-aliased (owned) or finalized.
  }
  if (b->pos >= b->last) {
    a->done = true;  // fully drained while aliased (fast client): spent.
    return NGX_OK;
  }
  // Stamp a fresh lease + Dekker-revalidate: a normal wrap whose lease-load is
  // seq_cst-after ours now defers; kCopyNow = a wrap is in flight (region
  // intact), kTorn = a wrap committed (region may be overwritten), kOk = none.
  const LeaseRenewal res = a->pin->RenewLeaseStrict();
  if (res == LeaseRenewal::kTorn) {
    a->done = true;
    if (a->renew_fail_stat != nullptr) {
      a->renew_fail_stat->Add(1);
    }
    return NGX_ERROR;  // torn tail, Content-Length already promised: RST.
  }
  const bool deadline_near = a->pin->NsUntilForcedWrap() <= kBarrierMarginNs;
  if (res == LeaseRenewal::kOk && !deadline_near) {
    return NGX_OK;  // provably safe to keep aliasing this drain.
  }
  // kCopyNow / kLeasesOff / a ceiling-forced wrap within the margin (which
  // ignores the lease): de-alias the unsent tail into request-owned memory
  // BEFORE the write filter reads it.  Single-threaded loop -> no race with
  // the write filter sending this same buf.
  size_t rem = static_cast<size_t>(b->last - b->pos);
  u_char* owned = static_cast<u_char*>(ngx_pnalloc(r->pool, rem));
  if (owned == nullptr) {
    a->done = true;
    return NGX_ERROR;  // cannot de-alias safely: fail closed (rare).
  }
  ngx_memcpy(owned, b->pos, rem);
  // copy-then-verify: a forced wrap could have raced the memcpy; re-check the
  // borrow AFTER the copy and fail closed if the epoch moved (torn copy).
  if (a->pin->RenewLeaseStrict() == LeaseRenewal::kTorn) {
    a->done = true;
    if (a->renew_fail_stat != nullptr) {
      a->renew_fail_stat->Add(1);
    }
    return NGX_ERROR;
  }
  b->start = b->pos = owned;
  b->last = b->end = owned + rem;
  b->memory = 0;
  b->temporary = 1;
  a->done = true;
  if (a->copied_out_stat != nullptr) {
    a->copied_out_stat->Add(1);
  }
  return NGX_OK;
}

// Ring degrade margin: while refills remain, a ceiling-forced wrap
// (which ignores our lease) reachable within this window means the stripe
// is too hot to keep leaning on the pin -- copy the whole remaining tail
// out now.  Same value and rationale as the emit-time margin
// (kEmitMarginNs) in ngx_base_fetch.cc.
const uint64_t kRingDegradeMarginNs = 500ULL * 1000 * 1000;  // 500 ms

// Bounded-copy ring: copy the next window from the pinned region into
// the oldest slot (or, under wrap pressure, the whole remaining tail into a
// one-shot pool buffer, finishing the ring).  Copy-then-verify per window,
// the same protocol as the whole-body copy path: a forced wrap ignores our
// fresh lease and can race the memcpy, so the borrow is re-checked AFTER
// the bytes were copied and a kTorn verdict fails the serve closed
// (Content-Length is already on the wire).
ngx_int_t ps_bounded_ring_emit_next(ngx_http_request_t* r,
                                    PsBoundedCopyRing* ring,
                                    ngx_chain_t** out) {
  *out = nullptr;
  DCHECK(!ring->done);
  DCHECK(ring->cursor < ring->size);
  const size_t remaining = ring->size - ring->cursor;
  ngx_buf_t* b = ring->slot[ring->next_refill];
  const size_t capacity = static_cast<size_t>(b->end - b->start);
  const size_t n = remaining < capacity ? remaining : capacity;
  ngx_memcpy(b->start, ring->src + ring->cursor, n);
  const LeaseRenewal res = ring->pin->RenewLeaseStrict();
  if (res == LeaseRenewal::kTorn) {
    ring->done = true;
    if (ring->renew_fail_stat != nullptr) {
      ring->renew_fail_stat->Add(1);
    }
    return NGX_ERROR;  // epoch moved: the copied window may be torn.
  }
  if (res == LeaseRenewal::kOk &&
      ring->pin->NsUntilForcedWrap() > kRingDegradeMarginNs) {
    ngx_chain_t* cl = ngx_alloc_chain_link(r->pool);
    if (cl == nullptr) {
      ring->done = true;
      return NGX_ERROR;
    }
    // Recycle the slot buf in place: same descriptor, new window.
    b->pos = b->start;
    b->last = b->start + n;
    b->flush = 0;
    b->shadow = nullptr;
    ring->cursor += n;
    ring->next_refill = (ring->next_refill + 1) % kRingSlotCount;
    if (ring->cursor == ring->size) {
      b->last_buf = 1;
      ring->done = true;
      // Every body byte is in nginx-owned memory now: stop pinning the
      // mapped region without waiting for the final window to drain.
      delete ring->pin;
      ring->pin = nullptr;
    } else {
      b->last_buf = 0;
    }
    if (ring->refills_stat != nullptr) {
      ring->refills_stat->Add(1);
    }
    cl->buf = b;
    cl->next = nullptr;
    *out = cl;
    return NGX_OK;
  }
  // kCopyNow (wrap in flight), kLeasesOff (no verify semantics to lean on),
  // or a ceiling-forced wrap within the margin: stop leaning on the pin.
  // Copy the whole remaining tail once, verify once more, and finish the
  // serve from owned memory (the just-copied slot window is discarded --
  // the tail starts at the unadvanced cursor and includes those bytes).
  u_char* owned = static_cast<u_char*>(ngx_pnalloc(r->pool, remaining));
  ngx_buf_t* tb = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
  ngx_chain_t* cl = ngx_alloc_chain_link(r->pool);
  if (owned == nullptr || tb == nullptr || cl == nullptr) {
    ring->done = true;
    return NGX_ERROR;
  }
  ngx_memcpy(owned, ring->src + ring->cursor, remaining);
  const LeaseRenewal rv = ring->pin->RenewLeaseStrict();
  delete ring->pin;
  ring->pin = nullptr;
  ring->done = true;
  ring->cursor = ring->size;
  if (rv == LeaseRenewal::kTorn) {
    if (ring->renew_fail_stat != nullptr) {
      ring->renew_fail_stat->Add(1);
    }
    return NGX_ERROR;
  }
  tb->start = tb->pos = owned;
  tb->last = tb->end = owned + remaining;
  tb->temporary = 1;
  tb->last_buf = 1;
  if (ring->copied_out_stat != nullptr) {
    ring->copied_out_stat->Add(1);
  }
  cl->buf = tb;
  cl->next = nullptr;
  *out = cl;
  return NGX_OK;
}

// Refill timer handler: drive the request's write path exactly as a
// write event would -- the writer flushes r->out, re-enters the body-filter
// chain (our refill block) with NULL input, and finalizes the request when
// everything has drained.  ngx_http_run_posted_requests is the standard
// epilogue for request work driven from a raw event.
void ps_bounded_ring_refill_event(ngx_event_t* ev) {
  ngx_http_request_t* r = static_cast<ngx_http_request_t*>(ev->data);
  ngx_connection_t* c = r->connection;
  r->write_event_handler(r);
  ngx_http_run_posted_requests(c);
}

ngx_int_t ps_base_fetch_filter(ngx_http_request_t* r, ngx_chain_t* in) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);

  if (r->header_only) {
    return NGX_OK;
  }
  // Run the per-drain barrier BEFORE the base_fetch-null early return
  // below -- the aliased buf can still be in r->out after the NgxBaseFetch was
  // released mid-drain, and it must be revalidated/de-aliased before any
  // further writev.  Keyed only off the request-scoped ctx + pool-scoped pin.
  if (ctx != nullptr && ctx->zerocopy_alias != nullptr) {
    if (ps_zerocopy_barrier(r, ctx->zerocopy_alias) != NGX_OK) {
      return NGX_ERROR;
    }
  }
  // Bounded-copy ring refill.  This is the top body filter, so
  // ngx_http_writer re-enters it (in == NULL) whenever the stream becomes
  // writable again -- that re-entry is the ring's refill clock.  Like the
  // barrier above it must run before the base_fetch-null early return: the
  // ring outlives the NgxBaseFetch by design.  A slot whose buf reads
  // pos == last is fully accepted by the socket (verified against nginx
  // 1.30.3: h2 advances the ORIGINAL buf's pos only on DATA-frame send
  // completion, via the shadow back-pointer, and only then recycles the
  // shadow chunks; QUIC copies at consumption), so no downstream reference
  // to the previous window's bytes can remain and the slot is reusable.
  // Slots drain in emission order, so scan from the oldest and stop at the
  // first undrained one.
  if (ctx != nullptr && ctx->bounded_ring != nullptr &&
      !ctx->bounded_ring->done) {
    PsBoundedCopyRing* ring = ctx->bounded_ring;
    ngx_chain_t* refills = nullptr;
    ngx_chain_t** refill_tail = &refills;
    while (!ring->done) {
      ngx_buf_t* slot = ring->slot[ring->next_refill];
      if (slot->pos < slot->last) {
        break;  // oldest window still in flight downstream.
      }
      ngx_chain_t* cl = nullptr;
      if (ps_bounded_ring_emit_next(r, ring, &cl) != NGX_OK) {
        ps_set_buffered(r, false);
        return NGX_ERROR;  // torn window mid-stream: reset fail-closed.
      }
      *refill_tail = cl;
      refill_tail = &cl->next;
    }
    if (refills != nullptr) {
      // Refilled windows go after whatever is already being passed down.
      if (in == nullptr) {
        in = refills;
      } else {
        ngx_chain_t* tail = in;
        while (tail->next != nullptr) {
          tail = tail->next;
        }
        tail->next = refills;
      }
    }
    // Keep our buffered bit set while windows remain so ngx_http_writer
    // stays armed (ngx_http_set_write_handler keys on r->buffered) across
    // h2 window reopens; once the final window is handed down, r->out and
    // the stream's own buffering own completion.  Reuses the same bit
    // ps_base_fetch_handler holds while collecting: the ring only exists
    // after the base fetch was released, so ownership never overlaps.
    ps_set_buffered(r, !ring->done);
    // Re-arm the refill clock: fast cadence while windows are turning over,
    // backed off when the in-flight windows have not drained yet (a slow
    // client's resume rides the exhausted-writer path; the timer is only
    // its backstop).  Disarm on completion -- and on the error return
    // above, request teardown runs the pool cleanup, which disarms.
    if (!ring->done) {
      ngx_add_timer(&ring->refill_ev, refills != nullptr ? 1 : 25);
    } else if (ring->refill_ev.timer_set) {
      ngx_del_timer(&ring->refill_ev);
    }
  }
  if (ctx == nullptr || ctx->base_fetch == nullptr) {
    return ngx_http_next_body_filter(r, in);
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "http pagespeed write filter \"%V\"", &r->uri);

  // send response body
  if (in || r->connection->buffered) {
    ngx_int_t rc = ngx_http_next_body_filter(r, in);
    // We can't indicate that we are done yet, because we have an active base
    // fetch associated to this request.
    if (rc != NGX_OK) {
      return rc;
    }
  }

  return NGX_AGAIN;
}

// This runs on the nginx event loop in response to seeing the byte PageSpeed
// sent over the pipe to trigger the nginx-side code.  Copy whatever is ready
// from PageSpeed out to the browser (headers and/or body).
ngx_int_t ps_base_fetch_handler(ngx_http_request_t* r) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);
  ngx_int_t rc;
  ngx_chain_t* cl = nullptr;

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "ps fetch handler: %V", &r->uri);

  if (ngx_terminate || ngx_exiting) {
    ps_set_buffered(r, false);
    ps_release_base_fetch(ctx);
    return NGX_ERROR;
  }

  if (!r->header_sent) {
    int status_code = ctx->base_fetch->response_headers()->status_code();
    bool status_ok = (status_code != 0) && (status_code < 400);

    // Pass on error handling for non-success status codes to nginx for
    // responses where PSOL acts as a content generator (pagespeed resource,
    // ipro hit).
    // This generates nginx's default error responses, but also allows header
    // modules running after us to manipulate those responses.
    // Admin pages generate their own error responses (JSON), so we must not
    // delegate those to nginx's error handler.
    if (!status_ok && (ctx->base_fetch->base_fetch_type() != kHtmlTransform &&
                       ctx->base_fetch->base_fetch_type() != kIproLookup &&
                       ctx->base_fetch->base_fetch_type() != kAdminPage)) {
      ps_release_base_fetch(ctx);
      ngx_http_filter_finalize_request(r, nullptr, status_code);
      return NGX_DONE;
    }

    if (ctx->base_fetch->base_fetch_type() == kIproLookup && !status_ok) {
      // This pass only decides: the decline below lets the server answer
      // the request itself.  headers_out may already carry fields set on
      // this request, and they must survive the miss.
    } else if (ctx->preserve_caching_headers != kDontPreserveHeaders) {
      ngx_table_elt_t* header;
      NgxListIterator it(&(r->headers_out.headers.part));
      while ((header = it.Next()) != nullptr) {
        // We need to remember a few headers when ModifyCachingHeaders is off,
        // so we can send them unmodified in copy_response_headers_to_ngx().
        // This just sets the hash to 0 for all other headers. That way, we
        // avoid  some relatively complicated code to reconstruct these headers.
        if (!(STR_CASE_EQ_LITERAL(header->key, "Cache-Control") ||
              (ctx->preserve_caching_headers == kPreserveAllCachingHeaders &&
               (STR_CASE_EQ_LITERAL(header->key, "Etag") ||
                STR_CASE_EQ_LITERAL(header->key, "Date") ||
                STR_CASE_EQ_LITERAL(header->key, "Last-Modified") ||
                STR_CASE_EQ_LITERAL(header->key, "Expires"))))) {
          header->hash = 0;
          if (STR_CASE_EQ_LITERAL(header->key, "Location")) {
            // There's a possible issue with the location header, where setting
            // the hash to 0 is not enough. See:
            // https://github.com/nginx/nginx/blob/master/src/http/ngx_http_header_filter_module.c#L314
            r->headers_out.location = nullptr;
          }
        }
      }
    } else {
      // The charset filter runs ahead of this module in the chain and has
      // already made its decision; cleaning the header would silently drop
      // it from the rewritten document's Content-Type.
      const ngx_str_t charset = r->headers_out.charset;
      ngx_http_clean_header(r);
      if (ctx->base_fetch->base_fetch_type() == kHtmlTransform) {
        r->headers_out.charset = charset;
      }
    }
    // collect response headers from pagespeed
    rc = ctx->base_fetch->CollectHeaders(&r->headers_out);
    if (rc == NGX_ERROR) {
      ps_release_base_fetch(ctx);
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    // Headers for responses this module generates enter the chain at the
    // module's own position, so nginx's not-modified filter (which runs
    // ahead of it) never sees them.  Answer If-None-Match for the module's
    // own resources here, with the weak comparison a GET calls for; the 304
    // conversion mirrors nginx's own not-modified filter.  (The in-place
    // path needs nothing: the optimization library answers its
    // conditionals.)
    if (ctx->base_fetch->base_fetch_type() == kPageSpeedResource &&
        r->headers_out.status == NGX_HTTP_OK &&
        r->headers_in.if_none_match != nullptr &&
        r->headers_out.etag != nullptr &&
        NgxIfNoneMatchMatches(
            str_to_string_piece(r->headers_in.if_none_match->value),
            str_to_string_piece(r->headers_out.etag->value))) {
      r->headers_out.status = NGX_HTTP_NOT_MODIFIED;
      r->headers_out.status_line.len = 0;
      r->headers_out.content_type.len = 0;
      ngx_http_clear_content_length(r);
      ngx_http_clear_accept_ranges(r);
      if (r->headers_out.content_encoding != nullptr) {
        r->headers_out.content_encoding->hash = 0;
        r->headers_out.content_encoding = nullptr;
      }
    }

    // send response headers
    //
    // Responses the module generates enter the chain HERE, at the module's
    // own position: nginx's headers filter (expires, add_header) runs above
    // it and never sees them. What they carry is INHERITED instead: a
    // rewritten resource merges its input resource's non-caching headers
    // (ServerContext::MergeNonCachingResponseHeaders), a recorded in-place
    // response carries what the recorder saw, and a resource loaded from
    // file has nothing to inherit. The operator's lever for a header on
    // these responses is pagespeed AddResourceHeader.
    rc = ngx_http_next_header_filter(r);

    // standard nginx send header check see ngx_http_send_response
    if (rc == NGX_ERROR || rc > NGX_OK) {
      ps_release_base_fetch(ctx);
      return ngx_http_filter_finalize_request(r, nullptr, rc);
    }

    // for in_place_check_header_filter
    if (rc < NGX_OK && rc != NGX_AGAIN) {
      CHECK(rc == NGX_DONE);
      return NGX_DONE;
    }

    ps_set_buffered(r, true);
  }

  // collect response body from pagespeed
  // Pass the optimized content along to later body filters.
  // From Weibin: This function should be called mutiple times. Store the
  // whole file in one chain buffers is too aggressive. It could consume
  // too much memory in busy servers.

  rc = ctx->base_fetch->CollectAccumulatedWrites(&cl);
  ngx_log_error(NGX_LOG_DEBUG, ctx->r->connection->log, 0,
                "CollectAccumulatedWrites, %d", rc);

  if (rc == NGX_ERROR) {
    ps_set_buffered(r, false);
    ps_release_base_fetch(ctx);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  if (rc == NGX_AGAIN && cl == nullptr) {
    // there is no body buffer to send now.
    return NGX_AGAIN;
  }

  if (rc == NGX_OK) {
    ps_set_buffered(r, false);
    ps_release_base_fetch(ctx);
  }

  return ps_base_fetch_filter(r, cl);
}

void ps_base_fetch_filter_init() {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ps_base_fetch_filter;
}

}  // namespace ps_base_fetch

namespace {

// Setting headers in nginx is tricky because it's not just a matter of adding
// them to a list.  You also need to remove them if there's already one there,
// as well as setting the shortcut pointers (both upper case and lower case).
//
// Based on ngx_http_add_cache_control.
ngx_int_t ps_set_cache_control(ngx_http_request_t* r, char* cache_control) {
#if defined(nginx_version) && nginx_version >= 1023000
  ngx_table_elt_t* cc = r->headers_out.cache_control;

  if (cc == nullptr) {
    cc = reinterpret_cast<ngx_table_elt_t*>(
        ngx_list_push(&r->headers_out.headers));
    if (cc == nullptr) {
      return NGX_ERROR;
    }

    r->headers_out.cache_control = cc;
    cc->next = nullptr;

    cc->hash = 1;
    ngx_str_set(&cc->key, "Cache-Control");

  } else {
    for (cc = cc->next; cc; cc = cc->next) {
      cc->hash = 0;
    }

    cc = r->headers_out.cache_control;
    cc->next = nullptr;
  }
  cc->value.len = strlen(cache_control);
  cc->value.data = reinterpret_cast<u_char*>(cache_control);

#else
  // First strip existing cache-control headers.
  ngx_table_elt_t* header;
  NgxListIterator it(&(r->headers_out.headers.part));
  while ((header = it.Next()) != NULL) {
    if (STR_CASE_EQ_LITERAL(header->key, "Cache-Control")) {
      // Response headers with hash of 0 are excluded from the response.
      header->hash = 0;
    }
  }
  // Now add our new cache control header.
  if (r->headers_out.cache_control.elts == NULL) {
    ngx_int_t rc = ngx_array_init(&r->headers_out.cache_control, r->pool, 1,
                                  sizeof(ngx_table_elt_t*));
    if (rc != NGX_OK) {
      return NGX_ERROR;
    }
  }
  ngx_table_elt_t** cache_control_headers = static_cast<ngx_table_elt_t**>(
      ngx_array_push(&r->headers_out.cache_control));
  if (cache_control_headers == NULL) {
    return NGX_ERROR;
  }
  cache_control_headers[0] =
      static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
  if (cache_control_headers[0] == NULL) {
    return NGX_ERROR;
  }
  cache_control_headers[0]->hash = 1;
  ngx_str_set(&cache_control_headers[0]->key, "Cache-Control");
  cache_control_headers[0]->value.len = strlen(cache_control);
  cache_control_headers[0]->value.data =
      reinterpret_cast<u_char*>(cache_control);
#endif
  return NGX_OK;
}

// Returns false if the header wasn't found.  Otherwise sets cache_control and
// returns true;
bool ps_get_cache_control(ngx_http_request_t* r, GoogleString* cache_control) {
  // Use headers_out.cache_control instead of looking for Cache-Control in
  // headers_out.headers, because if an upstream sent multiple Cache-Control
  // headers they're already combined in headers_out.cache_control.
#if defined(nginx_version) && nginx_version >= 1023000
  ngx_table_elt_t* cc = r->headers_out.cache_control;
  bool first_segment = true;

  while (cc != nullptr) {
    if (cc->hash) {
      if (first_segment) {
        first_segment = false;
      } else {
        cache_control->append(", ");
      }
      cache_control->append(reinterpret_cast<char*>(cc->value.data),
                            cc->value.len);
    }
    cc = cc->next;
  }
#else
  auto ccp = static_cast<ngx_table_elt_t**>(r->headers_out.cache_control.elts);
  if (ccp == nullptr) {
    return false;  // Header not present.
  }
  bool first_segment = true;
  for (ngx_uint_t i = 0; i < r->headers_out.cache_control.nelts; i++) {
    if (ccp[i]->hash == 0) {
      continue;  // Elements with a hash of 0 are marked as excluded.
    }
    if (first_segment) {
      first_segment = false;
    } else {
      cache_control->append(", ");
    }
    cache_control->append(reinterpret_cast<char*>(ccp[i]->value.data),
                          ccp[i]->value.len);
  }
#endif
  return true;
}

template <class Headers>
void copy_headers_from_table(const ngx_list_t& from, Headers* to) {
  // Standard nginx idiom for iterating over a list.  See ngx_list.h
  ngx_uint_t i;
  const ngx_list_part_t* part = &from.part;
  const ngx_table_elt_t* header = static_cast<ngx_table_elt_t*>(part->elts);

  for (i = 0; /* void */; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }

      part = part->next;
      header = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    // Make sure we don't copy over headers that are unset.
    if (header[i].hash == 0) {
      continue;
    }
    StringPiece key = str_to_string_piece(header[i].key);
    StringPiece value = str_to_string_piece(header[i].value);

    to->Add(key, value);
  }
}
}  // namespace

void copy_response_headers_from_ngx(const ngx_http_request_t* r,
                                    ResponseHeaders* headers) {
  headers->set_major_version(r->http_version / 1000);
  headers->set_minor_version(r->http_version % 1000);
  copy_headers_from_table(r->headers_out.headers, headers);

  headers->set_status_code(r->headers_out.status);

  // Manually copy over the content type because it's not included in
  // request_->headers_out.headers.
  headers->Add(HttpAttributes::kContentType,
               str_to_string_piece(r->headers_out.content_type));

  // When we don't have a date header, set one with the current time.
  if (headers->Lookup1(HttpAttributes::kDate) == nullptr) {
    PosixTimer timer;
    headers->SetDate(timer.NowMs());
  }

  // TODO(oschaaf): ComputeCaching should be called in setupforhtml()?
  headers->ComputeCaching();
}

void copy_request_headers_from_ngx(const ngx_http_request_t* r,
                                   RequestHeaders* headers) {
  // TODO(chaizhenhua): only allow RewriteDriver::kPassThroughRequestAttributes?
  headers->set_major_version(r->http_version / 1000);
  headers->set_minor_version(r->http_version % 1000);
  copy_headers_from_table(r->headers_in.headers, headers);
}

// Whether a Vary field value lists the Accept-Encoding token, alone or among
// others (a comma-separated list; tokens compare case-insensitively).
static bool ps_vary_lists_accept_encoding(const ngx_str_t& value) {
  StringPieceVector tokens;
  SplitStringPieceToVector(str_to_string_piece(value), ",", &tokens, true);
  for (StringPiece token : tokens) {
    TrimWhitespace(&token);
    if (StringCaseEqual(token, "Accept-Encoding")) {
      return true;
    }
  }
  return false;
}

// PSOL produces caching headers that need some changes before we can send them
// out.  Make those changes and populate r->headers_out from pagespeed_headers.
ngx_int_t copy_response_headers_to_ngx(
    ngx_http_request_t* r, const ResponseHeaders& pagespeed_headers,
    PreserveCachingHeaders preserve_caching_headers) {
  ngx_http_headers_out_t* headers_out = &r->headers_out;
  headers_out->status = pagespeed_headers.status_code();

  ngx_int_t i;
  for (i = 0; i < pagespeed_headers.NumAttributes(); i++) {
    // For IPRO cache misses, these gs_ variables may point to freed memory
    // when nginx writes the headers to the output as the NgxBaseFetch instance
    // that owns this memory gets released during request processing. So we
    // copy these strings later on.
    const GoogleString& name_gs = pagespeed_headers.Name(i);
    const GoogleString& value_gs = pagespeed_headers.Value(i);

    if (preserve_caching_headers == kPreserveAllCachingHeaders) {
      if (StringCaseEqual(name_gs, "ETag") ||
          StringCaseEqual(name_gs, "Expires") ||
          StringCaseEqual(name_gs, "Date") ||
          StringCaseEqual(name_gs, "Last-Modified") ||
          StringCaseEqual(name_gs, "Cache-Control")) {
        continue;
      }
    } else if (preserve_caching_headers == kPreserveOnlyCacheControl) {
      // Retain the original Cache-Control header, but send the recomputed
      // values for all other cache-related headers.
      if (StringCaseEqual(name_gs, "Cache-Control")) {
        continue;
      }
    }  // else we don't preserve any headers.

    ngx_str_t name, value;
    value.len = value_gs.size();
    value.data = reinterpret_cast<u_char*>(
        string_piece_to_pool_string(r->pool, value_gs.c_str()));

    name.len = name_gs.size();
    name.data = reinterpret_cast<u_char*>(
        string_piece_to_pool_string(r->pool, name_gs.c_str()));

    // In case string_piece_to_pool_string failed:
    if (name.data == nullptr || value.data == nullptr) {
      return NGX_ERROR;
    }

    // TODO(jefftk): If we're setting a cache control header we'd like to
    // prevent any downstream code from changing it.  Specifically, if we're
    // serving a cache-extended resource the url will change if the resource
    // does and so we've given it a long lifetime.  If the site owner has done
    // something like set all css files to a 10-minute cache lifetime, that
    // shouldn't apply to our generated resources.  See Apache code in
    // net/instaweb/apache/header_util:AddResponseHeadersToRequest

    // Make copies of name and value to put into headers_out.
    if (STR_EQ_LITERAL(name, "Cache-Control")) {
      ps_set_cache_control(r, reinterpret_cast<char*>(value.data));
      continue;
    } else if (STR_EQ_LITERAL(name, "Content-Type")) {
      // Unlike all the other headers, content_type is just a string.
      headers_out->content_type = value;

      // We should not include the charset when determining content_type_len, so
      // scan for the ';' that marks the start of the charset part.
      for (ngx_uint_t i = 0; i < value.len; i++) {
        if (value.data[i] == ';') {
          break;
        }
        headers_out->content_type_len = i + 1;
      }

      // In ngx_http_test_content_type() nginx will allocate and calculate
      // content_type_lowcase if we leave it as null.
      headers_out->content_type_lowcase = nullptr;
      continue;
      // TODO(oschaaf): are there any other headers we should not try to
      // copy here?
    } else if (STR_EQ_LITERAL(name,
                              "Connection")) {  // NOLINT(bugprone-branch-clone)
      continue;
    } else if (STR_EQ_LITERAL(name, "Keep-Alive")) {
      continue;
    } else if (STR_EQ_LITERAL(name, "Transfer-Encoding")) {
      continue;
    } else if (STR_EQ_LITERAL(name, "Vary") &&
               ps_vary_lists_accept_encoding(value)) {
      // The module's own Vary already states the encoding token (alone or
      // among others): the etag filter withdraws the compressor's stamp so
      // the token is not stated a second time.
      ps_request_ctx_t* ctx = ps_get_request_context(r);
      ctx->psol_vary_accept_only = true;
    }

    ngx_table_elt_t* header =
        static_cast<ngx_table_elt_t*>(ngx_list_push(&headers_out->headers));
    if (header == nullptr) {
      return NGX_ERROR;
    }

    header->hash = 1;  // Include this header in the output.
    header->key.data = name.data;
    header->key.len = name.len;
    header->value.data = value.data;
    header->value.len = value.len;

    // Populate the shortcuts to commonly used headers.
    if (STR_EQ_LITERAL(name, "Date")) {
      headers_out->date = header;
    } else if (STR_EQ_LITERAL(name, "Etag")) {
      // Set the shortcut so nginx can handle conditional requests (304).
      // For .pagespeed. resources, gzip is disabled in the location block
      // so it won't strip the ETag.  For IPRO responses, gzip may strip
      // weak ETags, but IPRO ETag tests aren't run on nginx currently.
      headers_out->etag = header;
    } else if (STR_EQ_LITERAL(name, "Expires")) {
      headers_out->expires = header;
    } else if (STR_EQ_LITERAL(name, "Last-Modified")) {
      headers_out->last_modified = header;
    } else if (STR_EQ_LITERAL(name, "Location")) {
      ps_request_ctx_t* ctx = ps_get_request_context(r);
      if (ctx->location_field_set) {
        headers_out->location = header;
      }
    } else if (STR_EQ_LITERAL(name, "Server")) {
      headers_out->server = header;
    } else if (STR_EQ_LITERAL(name, "Content-Length")) {
      int64 len;
      CHECK(pagespeed_headers.FindContentLength(&len));
      headers_out->content_length_n = len;
      headers_out->content_length = header;
    } else if (STR_EQ_LITERAL(name, "Content-Encoding")) {
      headers_out->content_encoding = header;
    } else if (STR_EQ_LITERAL(name, "Refresh")) {
      headers_out->refresh = header;
    } else if (STR_EQ_LITERAL(name, "Content-Range")) {
      headers_out->content_range = header;
    } else if (STR_EQ_LITERAL(name, "Accept-Ranges")) {
      headers_out->accept_ranges = header;
    } else if (STR_EQ_LITERAL(name, "WWW-Authenticate")) {
      headers_out->www_authenticate = header;
    }
  }

  return NGX_OK;
}

namespace {

using ps_main_conf_t = struct {
  NgxRewriteDriverFactory* driver_factory;
  MessageHandler* handler;
};

using ps_srv_conf_t = struct {
  // If pagespeed is configured in some server block but not this one our
  // per-request code will be invoked but server context will be null.  In those
  // cases we need to short circuit, not changing anything.  Currently our
  // header filter, body filter, and content handler all do this, but if anyone
  // adds another way for nginx to give us a request to process we need to check
  // there as well.
  NgxServerContext* server_context;
  ProxyFetchFactory* proxy_fetch_factory;
  // Only used while parsing config.  After we merge cfg_s and cfg_m you most
  // likely want cfg_s->server_context->config() as options here will be NULL.
  NgxRewriteOptions* options;
  MessageHandler* handler;
  // The names this server{} block is configured with, copied while the
  // configuration is loaded (ps_merge_srv_conf).  nginx keeps a server
  // block's server_names array in the configuration's TEMPORARY pool, which
  // it destroys once the configuration is loaded, so a request must never
  // read that array: it reads this copy.  Null for a server block without
  // pagespeed options, which answers no request of ours.
  ConfiguredHostNames* host_names;
};

using ps_loc_conf_t = struct {
  NgxRewriteOptions* options;
  MessageHandler* handler;
};

namespace RequestRouting {
enum Response : std::uint8_t {
  kError,
  kStaticContent,
  kInvalidUrl,
  kPagespeedDisabled,
  kBeacon,
  kStatistics,
  kGlobalStatistics,
  kConsole,
  kMessages,
  kAdmin,
  kCachePurge,
  kGlobalAdmin,
  kPagespeedSubrequest,
  kErrorResponse,
  // Opt-in counter (experimental): GET/HEAD
  // /.well-known/webbotauth-counter when the mode is non-off. Off (default) never
  // matches -> the request falls through to normal handling (404).
  kWebBotAuthCounter,
  kResource,
};
}  // namespace RequestRouting

char* ps_main_configure(ngx_conf_t* cf, ngx_command_t* cmd, void* conf);
char* ps_srv_configure(ngx_conf_t* cf, ngx_command_t* cmd, void* conf);
char* ps_loc_configure(ngx_conf_t* cf, ngx_command_t* cmd, void* conf);

// We want NGX_CONF_MULTI for some very old versions:
//   https://github.com/apache/incubator-pagespeed-ngx/commit/66f1b9aa
// but it's gone in recent revisions, so provide a compat #define if needed
#ifndef NGX_CONF_MULTI
#define NGX_CONF_MULTI 0
#endif

// TODO(jud): Verify that all the offsets should be NGX_HTTP_SRV_CONF_OFFSET and
// not NGX_HTTP_LOC_CONF_OFFSET or NGX_HTTP_MAIN_CONF_OFFSET.
ngx_command_t ps_commands[] = {
    {ngx_string("pagespeed"),
     NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1 | NGX_CONF_MULTI | NGX_CONF_TAKE2 |
         NGX_CONF_TAKE3 | NGX_CONF_TAKE4 | NGX_CONF_TAKE5,
     ps_main_configure, NGX_HTTP_SRV_CONF_OFFSET, 0, nullptr},
    {ngx_string("pagespeed"),
     NGX_HTTP_SRV_CONF | NGX_CONF_TAKE1 | NGX_CONF_MULTI | NGX_CONF_TAKE2 |
         NGX_CONF_TAKE3 | NGX_CONF_TAKE4 | NGX_CONF_TAKE5,
     ps_srv_configure, NGX_HTTP_SRV_CONF_OFFSET, 0, nullptr},

    {ngx_string("pagespeed"),
     NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1 | NGX_CONF_MULTI |
         NGX_CONF_TAKE2 | NGX_CONF_TAKE3 | NGX_CONF_TAKE4 | NGX_CONF_TAKE5,
     ps_loc_configure, NGX_HTTP_SRV_CONF_OFFSET, 0, nullptr},

    ngx_null_command};

bool ps_disabled(ps_srv_conf_t* cfg_s) {
  return cfg_s->server_context == nullptr ||
         cfg_s->server_context->config()->unplugged();
}

void ps_ignore_sigpipe() {
  struct sigaction act;
  ngx_memzero(&act, sizeof(act));
  act.sa_handler = SIG_IGN;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  sigaction(SIGPIPE, &act, nullptr);
}

// Given a directory path that pagespeed needs, create it and set permissions so
// the worker can access, but only if needed.
char* ps_init_dir(const StringPiece& directive, const StringPiece& path,
                  ngx_conf_t* cf) {
  if (path.size() == 0 || path[0] != '/') {
    return string_piece_to_pool_string(
        cf->pool, StrCat(directive, " ", path, " must start with a slash"));
  }

  net_instaweb::StdioFileSystem file_system;
  net_instaweb::NullMessageHandler message_handler;
  GoogleString gs_path;
  path.CopyToString(&gs_path);
  if (!file_system.IsDir(gs_path.c_str(), &message_handler).is_true()) {
    if (!file_system.RecursivelyMakeDir(path, &message_handler)) {
      return string_piece_to_pool_string(
          cf->pool, StrCat(directive, " path ", path,
                           " does not exist and could not be created."));
    }
    // Directory created, but may not be readable by the worker processes.
  }

  if (geteuid() != 0) {
    return nullptr;  // We're not root, so we're staying whoever we are.
  }

  // chown if owner differs from nginx worker user.
  ngx_core_conf_t* ccf = reinterpret_cast<ngx_core_conf_t*>(
      ngx_get_conf(cf->cycle->conf_ctx, ngx_core_module));
  CHECK(ccf != nullptr);
  struct stat gs_stat;
  if (stat(gs_path.c_str(), &gs_stat) != 0) {
    return string_piece_to_pool_string(
        cf->pool, StrCat(directive, " ", path, " stat() failed"));
  }
  if (gs_stat.st_uid != ccf->user) {
    if (chown(gs_path.c_str(), ccf->user, ccf->group) != 0) {
      return string_piece_to_pool_string(
          cf->pool, StrCat(directive, " ", path, " unable to set permissions"));
    }
  }

  return nullptr;
}

// We support interpretation of nginx variables in some configuration settings,
// but we also need to support literal dollar signs in those same settings.
// Nginx has no good solution for this, so we define $dollar to expand to '$',
// which lets people include literal dollar signs if they need them.
ngx_int_t ps_dollar(ngx_http_request_t* r, ngx_http_variable_value_t* v,
                    uintptr_t data) {
  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;
  v->data = reinterpret_cast<u_char*>(const_cast<char*>("$"));
  v->len = 1;
  return NGX_OK;
}

// Parse the configuration option represented by cf and add it to options,
// creating options if necessary.
char* ps_configure(ngx_conf_t* cf, NgxRewriteOptions** options,
                   MessageHandler* handler,
                   net_instaweb::RewriteOptions::OptionScope option_scope) {
  // args[0] is always "pagespeed"; ignore it.
  ngx_uint_t n_args = cf->args->nelts - 1;

  // In ps_commands we only register 'pagespeed' as taking up to
  // five arguments, so this check should never fire.
  CHECK(n_args <= NGX_PAGESPEED_MAX_ARGS);
  StringPiece args[NGX_PAGESPEED_MAX_ARGS];

  ngx_str_t* value = static_cast<ngx_str_t*>(cf->args->elts);
  ngx_uint_t i;
  for (i = 0; i < n_args; i++) {
    args[i] = str_to_string_piece(value[i + 1]);
  }

  if (n_args == 1) {
    if (StringCaseEqual(args[0], "on")) {
      // safe to call if the setter is disabled
      g_gzip_setter.EnableGZipForLocation(cf);
    } else if (StringCaseEqual(args[0], "off")) {
      g_gzip_setter.SetGZipForLocation(cf, false);
    }
  }
  if (n_args == 2 && StringCaseEqual(args[0], "gzip")) {
    if (StringCaseEqual(args[1], "on")) {
      g_gzip_setter.SetGZipForLocation(cf, true);
    } else if (StringCaseEqual(args[1], "off")) {
      g_gzip_setter.SetGZipForLocation(cf, false);
    } else {
      char* error_message = string_piece_to_pool_string(
          cf->pool, StringPiece("Invalid pagespeed gzip setting"));
      return error_message;
    }
    return NGX_CONF_OK;
  }

  // Some options require the worker process to be able to read and write to
  // a specific directory.  Generally the master process is root while the
  // worker is nobody, so we need to change permissions and create the directory
  // if necessary.
  if (n_args == 2 &&
      (net_instaweb::StringCaseEqual("LogDir", args[0]) ||
       net_instaweb::StringCaseEqual("FileCachePath", args[0]))) {
    char* error_message = ps_init_dir(args[0], args[1], cf);
    if (error_message != nullptr) {
      return error_message;
    }
    // The directory has been prepared, but we haven't actually parsed the
    // directive yet.  That happens below in ParseAndSetOptions().
  }

  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_cycle_get_module_main_conf(cf->cycle, ngx_pagespeed));
  if (*options == nullptr) {
    *options = new NgxRewriteOptions(cfg_m->driver_factory->thread_system());
  }

  ProcessScriptVariablesMode script_mode =
      dynamic_cast<NgxRewriteDriverFactory*>(cfg_m->driver_factory)
          ->process_script_variables();
  if (script_mode != ProcessScriptVariablesMode::kOff) {
    // To be able to use '$', we map '$ps_dollar' to '$' via a script variable.
    ngx_str_t name = ngx_string("ps_dollar");
    ngx_http_variable_t* var =
        ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_CHANGEABLE);

    if (var == nullptr) {
      return const_cast<char*>(
          "Failed to add global configuration variable for '$ps_dollar'");
    }
    var->get_handler = ps_dollar;
  }

  const char* status = (*options)->ParseAndSetOptions(
      args, n_args, cf->pool, handler, cfg_m->driver_factory, option_scope, cf,
      script_mode);

  // nginx expects us to return a string literal but doesn't mark it const.
  return const_cast<char*>(status);
}

char* ps_main_configure(ngx_conf_t* cf, ngx_command_t* cmd, void* conf) {
  ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
      ngx_http_conf_get_module_srv_conf(cf, ngx_pagespeed));
  return ps_configure(cf, &cfg_s->options, cfg_s->handler,
                      net_instaweb::RewriteOptions::kProcessScopeStrict);
}

char* ps_srv_configure(ngx_conf_t* cf, ngx_command_t* cmd, void* conf) {
  ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
      ngx_http_conf_get_module_srv_conf(cf, ngx_pagespeed));
  return ps_configure(cf, &cfg_s->options, cfg_s->handler,
                      net_instaweb::RewriteOptions::kServerScope);
}

char* ps_loc_configure(ngx_conf_t* cf, ngx_command_t* cmd, void* conf) {
  ps_loc_conf_t* cfg_l = static_cast<ps_loc_conf_t*>(
      ngx_http_conf_get_module_loc_conf(cf, ngx_pagespeed));
  return ps_configure(cf, &cfg_l->options, cfg_l->handler,
                      net_instaweb::RewriteOptions::kDirectoryScope);
}

void ps_cleanup_loc_conf(void* data) {
  ps_loc_conf_t* cfg_l = static_cast<ps_loc_conf_t*>(data);
  delete cfg_l->handler;
  cfg_l->handler = nullptr;
  delete cfg_l->options;
  cfg_l->options = nullptr;
}

bool factory_deleted = false;
void ps_cleanup_srv_conf(void* data) {
  ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(data);

  // destroy the factory on the first call, causing all worker threads
  // to be shut down when we destroy any proxy_fetch_factories. This
  // will prevent any queued callbacks to destroyed proxy fetch factories
  // from being executed
  if (!factory_deleted && cfg_s->server_context != nullptr) {
    if (active_driver_factory == cfg_s->server_context->factory()) {
      active_driver_factory = nullptr;
    }
    delete cfg_s->server_context->factory();
    factory_deleted = true;
  }
  if (cfg_s->proxy_fetch_factory != nullptr) {
    delete cfg_s->proxy_fetch_factory;
    cfg_s->proxy_fetch_factory = nullptr;
  }
  delete cfg_s->handler;
  cfg_s->handler = nullptr;
  delete cfg_s->options;
  cfg_s->options = nullptr;
  delete cfg_s->host_names;
  cfg_s->host_names = nullptr;
}

void ps_cleanup_main_conf(void* data) {
  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(data);
  delete cfg_m->handler;
  cfg_m->handler = nullptr;
  NgxRewriteDriverFactory::Terminate();
  NgxRewriteOptions::Terminate();

  // reset the factory deleted flag, so we will clean up properly next time,
  // in case of a configuration reload.
  // TODO(oschaaf): get rid of the factory_deleted flag
  factory_deleted = false;
}

template <typename ConfT>
ConfT* ps_create_conf(ngx_conf_t* cf) {
  ConfT* cfg = static_cast<ConfT*>(ngx_pcalloc(cf->pool, sizeof(ConfT)));
  if (cfg == nullptr) {
    return nullptr;
  }
  cfg->handler = new GoogleMessageHandler();
  return cfg;
}

void ps_set_conf_cleanup_handler(ngx_conf_t* cf, void(func)(void*),
                                 void* data) {  // NOLINT
  ngx_pool_cleanup_t* cleanup_m = ngx_pool_cleanup_add(cf->pool, 0);
  if (cleanup_m == nullptr) {
    ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                       "failed to register a cleanup handler");
  } else {
    cleanup_m->handler = func;
    cleanup_m->data = data;
  }
}

void terminate_process_context() {
  if (active_driver_factory != nullptr) {
    // If we got here, that means we are in the cache loader/manager
    // or did not get a chance to cleanup otherwise.
    delete active_driver_factory;
    active_driver_factory = nullptr;
    NgxBaseFetch::Terminate();
  }
  delete process_context;
  process_context = nullptr;
}

void* ps_create_main_conf(ngx_conf_t* cf) {
  if (!process_context_cleanup_hooked) {
    // Note: InitApr() was removed - APR is no longer used
    atexit(terminate_process_context);
    process_context_cleanup_hooked = true;
  }
  ps_main_conf_t* cfg_m = ps_create_conf<ps_main_conf_t>(cf);
  if (cfg_m == nullptr) {
    return nullptr;
  }
  CHECK(!factory_deleted);
  NgxRewriteOptions::Initialize();
  NgxRewriteDriverFactory::Initialize();

  cfg_m->driver_factory = new NgxRewriteDriverFactory(
      *process_context, new SystemThreadSystem(), "" /* hostname, not used */,
      -1 /* port, not used */);
  active_driver_factory = cfg_m->driver_factory;
  active_driver_factory->LoggingInit(ngx_cycle->log, false);
  cfg_m->driver_factory->Init();
  ps_set_conf_cleanup_handler(cf, ps_cleanup_main_conf, cfg_m);
  return cfg_m;
}

void* ps_create_srv_conf(ngx_conf_t* cf) {
  ps_srv_conf_t* cfg_s = ps_create_conf<ps_srv_conf_t>(cf);
  if (cfg_s == nullptr) {
    return nullptr;
  }
  ps_set_conf_cleanup_handler(cf, ps_cleanup_srv_conf, cfg_s);
  return cfg_s;
}

void* ps_create_loc_conf(ngx_conf_t* cf) {
  ps_loc_conf_t* cfg_l = ps_create_conf<ps_loc_conf_t>(cf);
  if (cfg_l == nullptr) {
    return nullptr;
  }
  ps_set_conf_cleanup_handler(cf, ps_cleanup_loc_conf, cfg_l);
  return cfg_l;
}

// nginx has hierarchical configuration.  It maintains configurations at many
// levels.  At various points it needs to merge configurations from different
// levels, and then it calls this.  First it creates the configuration at the
// new level, parsing any pagespeed directives, then it merges in the
// configuration from the level above.  This function should merge the parent
// configuration into the child.  It's more complex than options->Merge() both
// because of the cases where the parent or child didn't have any pagespeed
// directives and because merging is order-dependent in the opposite way we'd
// like.
void ps_merge_options(NgxRewriteOptions* parent_options,
                      NgxRewriteOptions** child_options) {
  if (parent_options == nullptr) {
    // Nothing to do.
  } else if (*child_options == nullptr) {
    *child_options = parent_options->Clone();
  } else {  // Both non-null.
    // Unfortunately, merging configuration options is order dependent.  We'd
    // like to just do (*child_options)->Merge(*parent_options)
    // but then if we had:
    //    pagespeed RewriteLevel PassThrough
    //    server {
    //       pagespeed RewriteLevel CoreFilters
    //    }
    // it would always be stuck on PassThrough.
    NgxRewriteOptions* child_specific_options = *child_options;
    *child_options = parent_options->Clone();
    (*child_options)->Merge(*child_specific_options);

    if (child_specific_options->clear_inherited_scripts()) {
      // We don't want to inherit any inherited script lines from the parent
      // options here, so we just stick to the child specific ones.
      child_specific_options->CopyScriptLinesTo(*child_options);
    } else {
      // We append the child specific script lines to the parent's script lines
      // so we preserve the order in which they will be executed at request time
      child_specific_options->AppendScriptLinesTo(*child_options);
    }
    delete child_specific_options;
  }
}

namespace {

int times_ps_merge_srv_conf_called = 0;

// The names a server{} block is configured with: its server_name -- nginx's
// own primary name, a leading "." already dropped, a regular expression's
// "~" put back -- and every server_names entry that is not a regular
// expression, as written.  Wildcard and placeholder ("_") entries are passed
// as they are; VouchedServeHost never treats them as exact names.
//
// CONFIGURATION TIME ONLY.  cscf->server_names lives in the configuration's
// temporary pool (ngx_http_core_create_srv_conf), which nginx destroys when
// the configuration has been loaded; at request time the array points at
// freed memory.  The names are therefore copied here, once per server block,
// and requests read the copy (NgxConfiguredHostNames).
ConfiguredHostNames NgxCollectConfiguredHostNames(
    const ngx_http_core_srv_conf_t* cscf) {
  ConfiguredHostNames names;
  if (cscf == nullptr) {
    return names;
  }
  names.primary = str_to_string_piece(cscf->server_name).as_string();
  const ngx_http_server_name_t* server_names =
      static_cast<const ngx_http_server_name_t*>(cscf->server_names.elts);
  for (ngx_uint_t i = 0; i < cscf->server_names.nelts; ++i) {
    // nginx stores a regular-expression entry WITHOUT its "~": the stored
    // text is a pattern that can look like a host name, never a name, so
    // it is not passed at all.
#if (NGX_PCRE)
    if (server_names[i].regex != nullptr) {
      continue;
    }
#endif
    names.aliases.push_back(
        str_to_string_piece(server_names[i].name).as_string());
  }
  return names;
}

}  // namespace

// Called exactly once per server block to merge the main configuration with the
// configuration for this server.
char* ps_merge_srv_conf(ngx_conf_t* cf, void* parent, void* child) {
  times_ps_merge_srv_conf_called += 1;

  ps_srv_conf_t* parent_cfg_s = static_cast<ps_srv_conf_t*>(parent);
  ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(child);

  ps_merge_options(parent_cfg_s->options, &cfg_s->options);

  if (cfg_s->options == nullptr) {
    return NGX_CONF_OK;  // No pagespeed options; don't do anything.
  }

  // ServerContext needs a hostname and port, but I don't see how to get this
  // and it ignores that a server can have multiple names and ports.  Because
  // the server context only needs them to make a unique identifier and to make
  // debugging easier, substitute our own unique identifier.
  // TODO(jefftk): either figure out how to get a hostname and port for this
  // server block or change ServerContext not to ask for them.
  int dummy_port = -times_ps_merge_srv_conf_called;

  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_conf_get_module_main_conf(cf, ngx_pagespeed));
  cfg_m->driver_factory->SetMainConf(parent_cfg_s->options);
  cfg_s->server_context =
      cfg_m->driver_factory->MakeNgxServerContext("dummy_hostname", dummy_port);

  // Copy this server block's names now, while nginx's own list of them is
  // still alive (see NgxCollectConfiguredHostNames).  The core module's
  // merge has already run for this block, so its primary name is settled.
  delete cfg_s->host_names;
  cfg_s->host_names = new ConfiguredHostNames(
      NgxCollectConfiguredHostNames(static_cast<ngx_http_core_srv_conf_t*>(
          ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module))));

#if (NGX_HTTP_V2)
  // Save the variable index of the "http2" variable, so we can use it
  // at request time to lookup whether that's on. We do this conditionally
  // since NGINX will complain to the user (at [emerg] level!) if it doesn't
  // know about it.
  ngx_str_t name = ngx_string("http2");
  cfg_s->server_context->set_ngx_http2_variable_index(
      ngx_http_get_variable_index(cf, &name));
#endif

  // The server context sets some options when we call global_options(). So
  // let it do that, then merge in options we got from the config file.
  // Once we do that we're done with cfg_s->options.
  cfg_s->server_context->global_options()->Merge(*cfg_s->options);
  NgxRewriteOptions* ngx_options =
      dynamic_cast<NgxRewriteOptions*>(cfg_s->server_context->global_options());
  cfg_s->options->CopyScriptLinesTo(ngx_options);
  delete cfg_s->options;
  cfg_s->options = nullptr;

  if (!cfg_s->server_context->global_options()->unplugged()) {
    // Validate FileCachePath
    GoogleMessageHandler handler;
    const char* file_cache_path =
        cfg_s->server_context->config()->file_cache_path().c_str();
    if (file_cache_path[0] == '\0') {
      if (!cfg_s->server_context->global_options()->standby()) {
        return const_cast<char*>("FileCachePath must be set, even for standby");
      } else {
        return const_cast<char*>("FileCachePath must be set");
      }
    } else if (!cfg_m->driver_factory->file_system()
                    ->IsDir(file_cache_path, &handler)
                    .is_true()) {
      return const_cast<char*>(
          "FileCachePath must be an nginx-writeable directory");
    }
  }

  return NGX_CONF_OK;
}

char* ps_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child) {
  ps_loc_conf_t* cfg_l = static_cast<ps_loc_conf_t*>(child);
  if (cfg_l->options == nullptr) {
    // No directory specific options.
    return NGX_CONF_OK;
  }

  // While you can't put a "location" block inside a "location" block you can
  // put an "if" block inside a "location" block, which is implemented by making
  // a pretend "location" block.  In this case we may have pagespeed options
  // from the parent "location" block as well as from the current locationish
  // "if" block.
  ps_loc_conf_t* parent_cfg_l = static_cast<ps_loc_conf_t*>(parent);
  if (parent_cfg_l->options != nullptr) {
    // Rebase our options off of the ones defined in the parent location block.
    ps_merge_options(parent_cfg_l->options, &cfg_l->options);
    return NGX_CONF_OK;
  }

  // Pagespeed options are defined in this location block, and it either has no
  // parent (typical case) or is an if block whose parent location block defines
  // no pagespeed options.  Base our options off of those in the server block.

  ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
      ngx_http_conf_get_module_srv_conf(cf, ngx_pagespeed));

  if (ps_disabled(cfg_s)) {
    // Pagespeed options cannot be defined only in location blocks.  There must
    // be at least a single "pagespeed off" in the main block or a server
    // block.
    return NGX_CONF_OK;
  }

  // If we get here we have parent options ("global options") from cfg_s, child
  // options ("directory specific options") from cfg_l, and no options from
  // parent_cfg_l.  Rebase the directory specific options on the global options.
  ps_merge_options(cfg_s->server_context->config(), &cfg_l->options);

  return NGX_CONF_OK;
}

// _ef_ is a shorthand for ETag Filter
ngx_http_output_header_filter_pt ngx_http_ef_next_header_filter;

bool ps_is_https(ngx_http_request_t* r) {
  // Based on ngx_http_variable_scheme.
#if (NGX_HTTP_SSL)
  return r->connection->ssl;
#endif
  return false;
}

int ps_determine_port(ngx_http_request_t* r) {
  // Return -1 if the port isn't specified, the port number otherwise.
  //
  // If a Host header was provided, get the host from that.  Otherwise fall back
  // to the local port of the incoming connection.

  int port = -1;
  ngx_table_elt_t* host = r->headers_in.host;

  if (host != nullptr) {
    // Host headers can look like:
    //
    //   www.example.com        // normal
    //   www.example.com:8080   // port specified
    //   127.0.0.1              // IPv4
    //   127.0.0.1:8080         // IPv4 with port
    //   [::1]                  // IPv6
    //   [::1]:8080             // IPv6 with port
    //
    // The IPv6 ones are the annoying ones, but the square brackets allow us to
    // disambiguate.  To find the port number, we can say:
    //
    //   1) Take the text after the final colon.
    //   2) If all of those characters are digits, that's your port number
    //
    // In the case of a plain IPv6 address with no port number, the text after
    // the final colon will include a ']', so we'll stop processing.

    StringPiece host_str = str_to_string_piece(host->value);
    size_t colon_index = host_str.rfind(":");
    if (colon_index == host_str.npos) {
      return -1;
    }
    // Strip everything up to and including the final colon.
    host_str.remove_prefix(colon_index + 1);

    bool ok = StringToInt(host_str, &port);
    if (!ok) {
      // Might be malformed port, or just IPv6 with no port specified.
      return -1;
    }

    return port;
  }

  // Based on ngx_http_variable_server_port.
#if (NGX_HAVE_INET6)
  if (r->connection->local_sockaddr->sa_family == AF_INET6) {
    port = ntohs(
        reinterpret_cast<struct sockaddr_in6*>(r->connection->local_sockaddr)
            ->sin6_port);
  }
#endif
  if (port == -1 /* still need port */) {
    port = ntohs(
        reinterpret_cast<struct sockaddr_in*>(r->connection->local_sockaddr)
            ->sin_port);
  }

  return port;
}
}  // namespace

StringPiece ps_determine_host(ngx_http_request_t* r) {
  StringPiece host = str_to_string_piece(r->headers_in.server);
  if (host.size() == 0) {
    // If host is unspecified, perhaps because of a pure HTTP 1.0 "GET /path",
    // fall back to server IP address.  Based on ngx_http_variable_server_addr.
    // The buffer must come from r->pool, not the stack: callers read the
    // returned StringPiece after this frame is gone.
    ngx_str_t s;
    u_char* addr =
        static_cast<u_char*>(ngx_pnalloc(r->pool, NGX_SOCKADDR_STRLEN));
    if (addr == nullptr) {
      return host;
    }
    s.len = NGX_SOCKADDR_STRLEN;
    s.data = addr;
    ngx_int_t rc = ngx_connection_local_sockaddr(r->connection, &s, 0);
    if (rc != NGX_OK) {
      s.len = 0;
    }
    host = str_to_string_piece(s);
  }
  return host;
}

// External-linkage accessor, defined here (OUTSIDE the anonymous namespace
// below) so other translation units -- the Web-Bot-Auth glue in
// ngx_webbotauth_handler.cc -- can link against it. Mirrors the file-local
// ps_get_srv_config lookup but must NOT live in the anonymous namespace, or the
// dynamic module .so would dlopen with an undefined ps_get_server_context.
NgxServerContext* ps_get_server_context(ngx_http_request_t* r) {
  ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
      ngx_http_get_module_srv_conf(r, ngx_pagespeed));
  if (cfg_s == nullptr) {
    return nullptr;
  }
  return cfg_s->server_context;
}

namespace {

GoogleString ps_determine_url(ngx_http_request_t* r) {
  int port = ps_determine_port(r);
  GoogleString port_string;
  if ((ps_is_https(r) && (port == 443 || port == -1)) ||
      (!ps_is_https(r) && (port == 80 || port == -1))) {
    // No port specifier needed for requests on default ports.
    port_string = "";
  } else {
    port_string = StrCat(":", IntegerToString(port));
  }

  StringPiece host = ps_determine_host(r);

  return StrCat(ps_is_https(r) ? "https://" : "http://", host, port_string,
                str_to_string_piece(r->unparsed_uri));
}

// we are still at pagespeed phase
ngx_int_t ps_decline_request(ngx_http_request_t* r) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);
  CHECK(ctx != nullptr);

  ctx->driver->Cleanup();
  ctx->driver = nullptr;
  ctx->location_field_set = false;
  ctx->psol_vary_accept_only = false;

  // re init ctx
  ctx->html_rewrite = true;
  ctx->in_place = false;
  ps_release_base_fetch(ctx);
  ps_set_buffered(r, false);

  r->count++;
  r->phase_handler++;

  //restore read_event_handler to what it was in ps_async_wait_response
  r->read_event_handler = ngx_http_block_reading;
  r->write_event_handler = ngx_http_core_run_phases;
  ngx_http_core_run_phases(r);
  ngx_http_run_posted_requests(r->connection);
  return NGX_DONE;
}

ngx_int_t ps_async_wait_response(ngx_http_request_t* r) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);
  CHECK(ctx != nullptr);

  r->count++;
  // While we wait for PSOL to complete an async operation, there is a chance
  // that the underlying connection gets closed,  or a http/2 RST_STREAM is
  // received before the async operation completes. In that case we don't want
  // to continue processing this flow. So we override the requests's read event
  // handler with one that will make nginx abort request processing and execute
  // our cleanup handlers instead of resuming request processing.
  r->read_event_handler = ngx_http_test_reading;
  r->write_event_handler = ngx_http_request_empty_handler;
  ps_set_buffered(r, true);
  // We don't need to add a timer here, as it will be set by nginx.
  return NGX_DONE;
}

// Populate cfg_* with configuration information for this request.
// Thin wrappers around ngx_http_get_module_*_conf and cast.
ps_srv_conf_t* ps_get_srv_config(ngx_http_request_t* r) {
  return static_cast<ps_srv_conf_t*>(
      ngx_http_get_module_srv_conf(r, ngx_pagespeed));
}

ps_loc_conf_t* ps_get_loc_config(ngx_http_request_t* r) {
  return static_cast<ps_loc_conf_t*>(
      ngx_http_get_module_loc_conf(r, ngx_pagespeed));
}

RewriteOptions* ps_determine_remote_options(ps_srv_conf_t* cfg_s) {
  if (!cfg_s || !cfg_s->server_context ||
      !cfg_s->server_context->global_options()) {
    return nullptr;
  }
  if (!cfg_s->server_context->global_options()
           ->remote_configuration_url()
           .empty()) {
    RewriteOptions* remote_options =
        cfg_s->server_context->global_options()->Clone();
    // This fetch is blocking for up to remote_configuration_timeout_ms ms.
    cfg_s->server_context->GetRemoteOptions(remote_options, false);
    return remote_options;
  }
  return nullptr;
}

// The option scan in GetQueryOptions() strips the option headers it applied
// (PageSpeed, ModPagespeed, PageSpeedFilters, ...) from `scanned`, the copy
// copy_response_headers_from_ngx() made of r->headers_out, and never touches
// r->headers_out itself.  Mark every live r->headers_out entry that no longer
// has a name/value counterpart in `scanned` as unset (hash = 0), so the
// shared scanner alone decides what is stripped.  Only names the scanner can
// strip are eligible; no other header is touched.
void ps_remove_stripped_option_headers(ngx_http_request_t* r,
                                       int num_attributes_before_scan,
                                       const ResponseHeaders& scanned) {
  if (scanned.NumAttributes() >= num_attributes_before_scan) {
    return;  // Nothing was stripped.
  }
  std::vector<bool> matched(scanned.NumAttributes(), false);
  ngx_table_elt_t* header;
  NgxListIterator it(&(r->headers_out.headers.part));
  while ((header = it.Next()) != nullptr) {
    if (header->hash == 0) {
      continue;
    }
    const StringPiece name = str_to_string_piece(header->key);
    if (!RewriteQuery::MightBeCustomOption(name)) {
      continue;
    }
    const StringPiece value = str_to_string_piece(header->value);
    bool kept = false;
    for (int i = 0, n = scanned.NumAttributes(); i < n; ++i) {
      if (!matched[i] && StringCaseEqual(scanned.Name(i), name) &&
          scanned.Value(i) == value) {
        matched[i] = true;
        kept = true;
        break;
      }
    }
    if (!kept) {
      // Response headers with hash of 0 are excluded from the response.
      header->hash = 0;
    }
  }
}

// Wrapper around GetQueryOptions()
RewriteOptions* ps_determine_request_options(
    ngx_http_request_t* r,
    const RewriteOptions* domain_options, /* may be null */
    RequestHeaders* request_headers, ResponseHeaders* response_headers,
    const RequestContextPtr& request_context, ps_srv_conf_t* cfg_s,
    GoogleUrl* url, GoogleString* pagespeed_query_params,
    GoogleString* pagespeed_option_cookies) {
  // Sets option from request headers and url.
  RewriteQuery rewrite_query;
  const int num_response_attributes = response_headers->NumAttributes();
  const bool parsed = cfg_s->server_context->GetQueryOptions(
      request_context, domain_options, url, request_headers, response_headers,
      &rewrite_query);
  // The scan stripped the option headers from the in-memory copy only; drop
  // them from the response itself too, before any caller returns early.
  ps_remove_stripped_option_headers(r, num_response_attributes,
                                    *response_headers);
  if (!parsed) {
    // Failed to parse query params or request headers.  Treat this as if there
    // were no query params given.
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "ps_determine_request_options: parsing headers or query "
                  "params failed.");
    return nullptr;
  }

  *pagespeed_query_params =
      rewrite_query.pagespeed_query_params().ToEscapedString();
  *pagespeed_option_cookies =
      rewrite_query.pagespeed_option_cookies().ToEscapedString();

  // Will be NULL if there aren't any options set with query params or in
  // headers.
  return rewrite_query.ReleaseOptions();
}

// Check whether this visitor is already in an experiment.  If they're not,
// classify them into one by setting a cookie.  Then set options appropriately
// for their experiment.
//
// See InstawebContext::SetExperimentStateAndCookie()
bool ps_set_experiment_state_and_cookie(ngx_http_request_t* r,
                                        RequestHeaders* request_headers,
                                        RewriteOptions* options,
                                        const StringPiece& host) {
  CHECK(options->running_experiment());
  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  bool need_cookie =
      cfg_s->server_context->experiment_matcher()->ClassifyIntoExperiment(
          *request_headers, *cfg_s->server_context->user_agent_matcher(),
          options);
  if (need_cookie && host.length() > 0) {
    PosixTimer timer;
    int64 time_now_ms = timer.NowMs();
    int64 expiration_time_ms =
        (time_now_ms + options->experiment_cookie_duration_ms());

    // TODO(jefftk): refactor SetExperimentCookie to expose the value we want to
    // set on the cookie.
    int state = options->experiment_id();
    GoogleString expires;
    ConvertTimeToString(expiration_time_ms, &expires);
    GoogleString value = absl::StrFormat(
        "%s=%s; Expires=%s; Domain=.%s; Path=/", experiment::kExperimentCookie,
        experiment::ExperimentStateToCookieString(state).c_str(),
        expires.c_str(), host.as_string().c_str());

    // Set the PagespeedExperiment cookie.
    ngx_table_elt_t* cookie =
        static_cast<ngx_table_elt_t*>(ngx_list_push(&r->headers_out.headers));
    if (cookie == nullptr) {
      return false;
    }
    cookie->hash = 1;  // Include this header in the response.

    ngx_str_set(&cookie->key, "Set-Cookie");
    // It's not safe to use value.c_str here because cookie header only keeps a
    // pointer to the string data.
    cookie->value.data =
        reinterpret_cast<u_char*>(string_piece_to_pool_string(r->pool, value));
    cookie->value.len = value.size();
  }
  return true;
}

// There are many sources of options:
//  - the request (query parameters, headers, and cookies)
//  - location block
//  - global server options
//  - experiment framework
// Consider them all, returning appropriate options for this request, of which
// the caller takes ownership.  If the only applicable options are global,
// set options to NULL so we can use server_context->global_options().
bool ps_determine_options(
    ngx_http_request_t* r, RequestHeaders* request_headers,
    ResponseHeaders* response_headers, RewriteOptions** options,
    const RequestContextPtr& request_context, ps_srv_conf_t* cfg_s,
    GoogleUrl* url, GoogleString* pagespeed_query_params,
    GoogleString* pagespeed_option_cookies, bool html_rewrite) {
  ps_loc_conf_t* cfg_l = ps_get_loc_config(r);

  // Global options for this server.  Never null.
  RewriteOptions* global_options = cfg_s->server_context->global_options();

  // Directory-specific options, usually null.  They've already been rebased off
  // of the global options as part of the configuration process.
  RewriteOptions* directory_options = cfg_l->options;

  // Request-specific options, nearly always null.  If set they need to be
  // rebased on the directory options or the global options.
  RewriteOptions* request_options = ps_determine_request_options(
      r, directory_options, request_headers, response_headers, request_context,
      cfg_s, url, pagespeed_query_params, pagespeed_option_cookies);
  bool have_request_options = request_options != nullptr;

  // Because the caller takes ownership of any options we return, the only
  // situation in which we can avoid allocating a new RewriteOptions is if the
  // global options are ok as they are and we don't have script variables we
  // need to evaluate at this point.
  NgxRewriteOptions* ngx_global_options =
      dynamic_cast<NgxRewriteOptions*>(global_options);
  if (!have_request_options && directory_options == nullptr &&
      !global_options->running_experiment() &&
      ngx_global_options->script_lines().size() == 0) {
    return true;
  }

  // Start with directory options if we have them, otherwise request options.
  if (directory_options != nullptr) {
    if (*options != nullptr) {
      (*options)->Merge(*directory_options);
    } else {
      *options = directory_options->Clone();
    }
  } else {
    if (*options == nullptr) {
      *options = global_options->Clone();
    }
  }

  NgxRewriteDriverFactory* ngx_factory =
      dynamic_cast<NgxRewriteDriverFactory*>(cfg_s->server_context->factory());
  NgxRewriteOptions* ngx_options = dynamic_cast<NgxRewriteOptions*>(*options);

  // ExecuteScriptVariables() sets 'pagespeed off' on ngx_options when execution
  // fails and then returns false. When that happens we return, as we don't want
  // to allow enabling pagespeed by request and execute without the intended
  // configuration.
  if (!ngx_options->ExecuteScriptVariables(r, cfg_s->handler, ngx_factory)) {
    return false;
  }

  // Modify our options in response to request options if specified.
  if (have_request_options) {
    (*options)->Merge(*request_options);
    delete request_options;
    request_options = nullptr;
  }

  // If we're running an experiment and processing html then modify our options
  // in response to the experiment.  Except we generally don't want experiments
  // to be contaminated with unexpected settings, so ignore experiments if we
  // have request-specific options.  Unless EnrollExperiment is on, probably set
  // by a query parameter, in which case we want to go ahead and apply the
  // experimental settings even if it means bad data, because we're just seeing
  // what it looks like.
  if ((*options)->running_experiment() && html_rewrite &&
      (!have_request_options || (*options)->enroll_experiment())) {
    bool ok = ps_set_experiment_state_and_cookie(r, request_headers, *options,
                                                 url->Host());
    if (!ok) {
      delete *options;
      *options = nullptr;
      return false;
    }
  }

  return true;
}

// Fix URL based on X-Forwarded-Proto.
// http://code.google.com/p/modpagespeed/issues/detail?id=546 For example, if
// Apache gives us the URL "http://www.example.com/" and there is a header:
// "X-Forwarded-Proto: https", then we update this base URL to
// "https://www.example.com/".  This only ever changes the protocol of the url.
//
// Returns true if it modified url, false otherwise.
bool ps_apply_x_forwarded_proto(ngx_http_request_t* r, GoogleString* url) {
  // First check for an X-Forwarded-Proto header.
  const ngx_str_t* x_forwarded_proto_header = nullptr;

  ngx_table_elt_t* header;
  NgxListIterator it(&(r->headers_in.headers.part));
  while ((header = it.Next()) != nullptr) {
    if (STR_CASE_EQ_LITERAL(header->key, "X-Forwarded-Proto")) {
      x_forwarded_proto_header = &header->value;
      break;
    }
  }

  if (x_forwarded_proto_header == nullptr) {
    return false;  // No X-Forwarded-Proto header found.
  }

  StringPiece x_forwarded_proto =
      str_to_string_piece(*x_forwarded_proto_header);
  if (!STR_CASE_EQ_LITERAL(*x_forwarded_proto_header, "http") &&
      !STR_CASE_EQ_LITERAL(*x_forwarded_proto_header, "https")) {
    LOG(WARNING) << "Unsupported X-Forwarded-Proto: " << x_forwarded_proto
                 << " for URL " << url << " protocol not changed.";
    return false;
  }

  StringPiece url_sp(*url);
  StringPiece::size_type colon_pos = url_sp.find(":");

  if (colon_pos == StringPiece::npos) {
    return false;  // URL appears to have no protocol; give up.
  }

  // Replace URL protocol with that specified in X-Forwarded-Proto.
  *url = StrCat(x_forwarded_proto, url_sp.substr(colon_pos));

  return true;
}

bool is_pagespeed_subrequest(ngx_http_request_t* r) {
  ngx_table_elt_t* user_agent_header = r->headers_in.user_agent;
  if (user_agent_header == nullptr) {
    return false;
  }
  StringPiece user_agent = str_to_string_piece(user_agent_header->value);
  return (user_agent.find(kModPagespeedSubrequestUserAgent) != user_agent.npos);
}

void ps_release_base_fetch(ps_request_ctx_t* ctx) {
  // In the normal flow BaseFetch doesn't delete itself in HandleDone() because
  // we still need to receive notification via pipe and call
  // CollectAccumulatedWrites.  If there's an error and we're cleaning up early
  // then HandleDone() hasn't been called yet and we need the base fetch to wait
  // for that and then delete itself.
  if (ctx->base_fetch != nullptr) {
    ctx->base_fetch->Detach();
    ctx->base_fetch = nullptr;
  }
}

// TODO(chaizhenhua): merge into NgxBaseFetch ctor
void ps_create_base_fetch(StringPiece url, ps_request_ctx_t* ctx,
                          const RequestContextPtr& request_context,
                          RequestHeaders* request_headers,
                          NgxBaseFetchType type,
                          const RewriteOptions* options) {
  CHECK(ctx->base_fetch == nullptr) << "Pre-existing base fetch!";

  ngx_http_request_t* r = ctx->r;
  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);

  // Handles its own deletion.  We need to call Release when we're done with
  // it, and call Done() on the associated parent (Proxy or Resource) fetch. If
  // we fail before creating the associated fetch then we need to call Done() on
  // the BaseFetch ourselves.
  ctx->base_fetch =
      new NgxBaseFetch(url, r, cfg_s->server_context, request_context,
                       ctx->preserve_caching_headers, type, options);
  ctx->base_fetch->SetRequestHeadersTakingOwnership(request_headers);
}

void ps_release_request_context(void* data) {
  ps_request_ctx_t* ctx = static_cast<ps_request_ctx_t*>(data);

  // proxy_fetch deleted itself if we called Done(), but if an error happened
  // before then we need to tell it to delete itself.
  //
  // If this is a resource fetch then proxy_fetch was never initialized.
  if (ctx->proxy_fetch != nullptr) {
    ctx->proxy_fetch->Done(false /* failure */);
    ctx->proxy_fetch = nullptr;
  }

  if (ctx->inflater_ != nullptr) {
    delete ctx->inflater_;
    ctx->inflater_ = nullptr;
  }

  if (ctx->driver != nullptr) {
    ctx->driver->Cleanup();
    ctx->driver = nullptr;
  }

  if (ctx->recorder != nullptr) {
    // Deletes recorder.
    ctx->recorder->DoneAndSetHeaders(nullptr, false /* incomplete response */);
    ctx->recorder = nullptr;
  }

  ps_release_base_fetch(ctx);
  delete ctx;
}

// Notes that a request was left to the server although a handler path
// matched it.  Reported at most once a minute per worker, on the cycle log:
// the request itself is deliberately not part of the message.
void ps_note_admin_route_declined() {
  static time_t last_report = 0;
  static ngx_uint_t unreported = 0;
  ++unreported;
  const time_t now = ngx_time();
  if (last_report != 0 && now - last_report < 60) {
    return;
  }
  ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                "pagespeed: left %ui request(s) to the server: the server "
                "and the module did not select the same handler for them",
                unreported);
  last_report = now;
  unreported = 0;
}

// Routes admin-style URLs: statistics, global statistics, console, messages,
// admin, and global admin. Returns nullopt if the request selects none.
std::optional<RequestRouting::Response> ps_route_admin_url(
    ngx_http_request_t* r, const GoogleUrl& url,
    const NgxRewriteOptions* options, bool client_is_loopback) {
  // The decision lives in NgxDecideAdminRoute (unit-tested).  The handler is
  // selected from nginx's own normalized URI -- the one the `location`
  // blocks that guard these paths were matched against -- with `location`
  // semantics: exact for every handler, plus the subtree below the two admin
  // handlers.  The path of the URL parsed for this request has to select the
  // same handler; a request nginx redirected internally and a subrequest are
  // left to the server.
  NgxAdminHandlerPaths paths;
  paths.statistics = options->statistics_path();
  paths.global_statistics = options->global_statistics_path();
  paths.console = options->console_path();
  paths.messages = options->messages_path();
  paths.admin = options->admin_path();
  paths.global_admin = options->global_admin_path();
  const StringPiece server_uri = str_to_string_piece(r->uri);
  const StringPiece module_path = url.PathSansQuery();
  const bool server_redirected = r->internal || r != r->main;

  // A handler whose own access check refuses the request is taken out and
  // the lookup repeated, so a later handler on a matching path still gets
  // its turn.  Each pass removes one handler: at most six passes.
  for (;;) {
    const NgxAdminRoute route =
        NgxDecideAdminRoute(server_uri, module_path, server_redirected, paths);
    if (route.declined) {
      ps_note_admin_route_declined();
      return std::nullopt;
    }
    switch (route.handler) {
      case NgxAdminHandler::kNone:
        return std::nullopt;
      case NgxAdminHandler::kStatistics:
        if (options->StatisticsAccessAllowed(url, client_is_loopback)) {
          return RequestRouting::kStatistics;
        }
        paths.statistics = StringPiece();
        break;
      case NgxAdminHandler::kGlobalStatistics:
        if (options->GlobalStatisticsAccessAllowed(url, client_is_loopback)) {
          return RequestRouting::kGlobalStatistics;
        }
        paths.global_statistics = StringPiece();
        break;
      case NgxAdminHandler::kConsole:
        if (options->ConsoleAccessAllowed(url, client_is_loopback)) {
          return RequestRouting::kConsole;
        }
        paths.console = StringPiece();
        break;
      case NgxAdminHandler::kMessages:
        if (options->MessagesAccessAllowed(url, client_is_loopback)) {
          return RequestRouting::kMessages;
        }
        paths.messages = StringPiece();
        break;
      case NgxAdminHandler::kAdmin:
        if (options->AdminAccessAllowed(url, client_is_loopback)) {
          return RequestRouting::kAdmin;
        }
        paths.admin = StringPiece();
        break;
      case NgxAdminHandler::kGlobalAdmin:
        if (options->GlobalAdminAccessAllowed(url, client_is_loopback)) {
          return RequestRouting::kGlobalAdmin;
        }
        paths.global_admin = StringPiece();
        break;
    }
  }
}

bool ps_is_cache_purge_request(ngx_http_request_t* r,
                               const NgxRewriteOptions* options) {
  return options->enable_cache_purge() && !options->purge_method().empty() &&
         options->purge_method() == str_to_string_piece(r->method_name);
}

bool ps_is_beacon_request(ngx_http_request_t* r, const GoogleUrl& url,
                          const NgxRewriteOptions* options) {
  const GoogleString& beacon_url =
      ps_is_https(r) ? options->beacon_url().https : options->beacon_url().http;
  return url.PathSansQuery() == StringPiece(beacon_url);
}

// Returns the textual client IP for `r`. nginx pre-populates
// connection->addr_text on accept; copy that ngx_str_t into GoogleString.
// Empty if unavailable.
GoogleString ps_client_ip_for_admin_warning(ngx_http_request_t* r) {
  if (r == nullptr || r->connection == nullptr) {
    return GoogleString();
  }
  const ngx_str_t& addr = r->connection->addr_text;
  if (addr.data == nullptr || addr.len == 0) {
    return GoogleString();
  }
  return GoogleString(reinterpret_cast<const char*>(addr.data), addr.len);
}

// Emit the shared admin-exposure warning for an nginx admin request, mapping
// the RequestRouting category to the AdminHandlerFamily + URL handler name.
void ps_warn_admin_exposure(ngx_http_request_t* r,
                            NgxServerContext* server_context,
                            RequestRouting::Response category) {
  AdminHandlerFamily family = AdminHandlerFamily::kAdmin;
  const char* name = "pagespeed_admin";
  switch (category) {
    case RequestRouting::kStatistics:
      family = AdminHandlerFamily::kStatistics;
      name = "mod_pagespeed_statistics";
      break;
    case RequestRouting::kGlobalStatistics:
      family = AdminHandlerFamily::kGlobalStatistics;
      name = "mod_pagespeed_global_statistics";
      break;
    case RequestRouting::kConsole:
      family = AdminHandlerFamily::kConsole;
      name = "pagespeed_console";
      break;
    case RequestRouting::kMessages:
      family = AdminHandlerFamily::kMessages;
      name = "mod_pagespeed_message";
      break;
    case RequestRouting::kAdmin:
      family = AdminHandlerFamily::kAdmin;
      name = "pagespeed_admin";
      break;
    case RequestRouting::kGlobalAdmin:
      family = AdminHandlerFamily::kGlobalAdmin;
      name = "pagespeed_global_admin";
      break;
    default:
      // Defensive: only the admin/statistics families should reach here.
      break;
  }
  WarnIfNonLoopbackAdminAccess(ps_client_ip_for_admin_warning(r), family, name,
                               server_context->message_handler());
}

// Set us up for processing a request.  Creates a request context and determines
// which handler should deal with the request.
RequestRouting::Response ps_route_request(ngx_http_request_t* r) {
  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);

  if (ps_disabled(cfg_s)) {
    // Not enabled for this server block.
    return RequestRouting::kPagespeedDisabled;
  }

  if (ngx_terminate || ngx_exiting) {
    return RequestRouting::kError;
  }
  if (r->err_status != 0) {
    return RequestRouting::kErrorResponse;
  }

  GoogleString url_string = ps_determine_url(r);
  GoogleUrl url(url_string);

  if (!url.IsWebValid()) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "invalid url");

    // Let nginx deal with the error however it wants; we will see a NULL ctx in
    // the body filter or content handler and do nothing.
    return RequestRouting::kInvalidUrl;
  }

  if (is_pagespeed_subrequest(r)) {
    return RequestRouting::kPagespeedSubrequest;
  }
  if (url.PathSansLeaf() ==
      dynamic_cast<NgxRewriteDriverFactory*>(cfg_s->server_context->factory())
          ->static_asset_prefix()) {
    return RequestRouting::kStaticContent;
  }

  const NgxRewriteOptions* global_options = cfg_s->server_context->config();

  // Strict-admin gate (opt-in, default off): decide loopback from the validated
  // connection IP, never the client-controlled Host header. With
  // StrictAdminAccess disabled the 3-arg AccessAllowed() overloads behave
  // exactly like the 2-arg forms, so this is a no-op for existing configs.
  const bool client_is_loopback =
      IsLoopbackClientIp(ps_client_ip_for_admin_warning(r));
  if (auto admin_response =
          ps_route_admin_url(r, url, global_options, client_is_loopback)) {
    return *admin_response;
  }
  if (ps_is_cache_purge_request(r, global_options)) {
    return RequestRouting::kCachePurge;
  }
  if (ps_is_beacon_request(r, url, global_options)) {
    return RequestRouting::kBeacon;
  }

  // Opt-in counter endpoint (experimental). Only match GET/HEAD
  // when the mode is non-off; POST/other methods and mode==off fall through to
  // normal handling (so they 404). The token/coarse-vs-exact/hide decision is
  // made in the handler. The fixed well-known path is not an operator option.
  {
    const GoogleString& counter_mode =
        global_options->web_bot_auth_public_counter();
    if ((counter_mode == "public" || counter_mode == "private") &&
        (r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD)) &&
        url.PathSansQuery() == "/.well-known/webbotauth-counter") {
      return RequestRouting::kWebBotAuthCounter;
    }
  }

  return RequestRouting::kResource;
}

// Forward declaration — defined later in this file.
bool ps_request_body_to_string_piece(ngx_http_request_t* r, StringPiece* out);

// Feeds the Web-Bot-Auth verdict already computed at PREACCESS into PageSpeed's
// own bot detection, so a signed agent presenting a browser user-agent is
// classified as the automated client it is. Gated on the default-off
// WebBotAuthBotDetection directive: without it, the verdict keeps reaching
// nothing but $x_verified_bot and the opt-in counters, which is the
// observe-only contract every existing WebBotAuth deployment relies on.
//
// Reads the stored verdict only -- no verification work happens here. Must be
// called after SetRequestHeaders, which recreates the driver's
// RequestProperties and would otherwise discard the verdict. Both directives
// are kServerScope, so the server config is the right place to read them, and
// it is the same config the preaccess handler and the $x_verified_bot variable
// consult.
void ps_apply_webbotauth_verdict(ngx_http_request_t* r, ps_srv_conf_t* cfg_s,
                                 RewriteDriver* driver) {
  const NgxRewriteOptions* config = cfg_s->server_context->config();
  if (config == nullptr || !config->web_bot_auth() ||
      !config->web_bot_auth_bot_detection()) {
    return;
  }
  if (ps_webbotauth_signature_verified(r)) {
    driver->SetWebBotAuthVerdict(true);
  }
}

namespace {

// The request-side strings the daemon serve request borrows from.  The
// request outlives the one Serve() call they feed; the record arm keeps its
// own copies for the same reason (the recorder outlives the handler).
struct PsDaemonServeRequestStrings {
  GoogleString url;
  GoogleString host;
  GoogleString scheme;
  GoogleString accept;
  GoogleString user_agent;
  GoogleString save_data;
  GoogleString accept_encoding;
  GoogleString if_none_match;
  GoogleString if_modified_since;
};

// Fills the daemon serve request the way the record arm fills its own: the
// URL the rest of the module already agreed on (PageSpeed parameters
// stripped, X-Forwarded-Proto honoured -- the SAME source the record key
// uses), the four capability headers verbatim with absent meaning empty,
// the client's conditionals verbatim, and the forced-reload test Apache
// applies (a case-insensitive "no-cache" in Cache-Control or Pragma).
//
// The headers and the URL come in ALREADY PARSED from the resource handler:
// re-reading them off the request would be a second RequestHeaders
// construction and a second URL parse on every in-place request, including
// every fall-through.
void BuildDaemonServeRequest(const RequestHeaders& request_headers,
                             const GoogleUrl& full_url,
                             PsDaemonServeRequestStrings* strings,
                             DaemonServeRequest* out) {
  full_url.PathAndLeaf().CopyToString(&strings->url);
  full_url.Host().CopyToString(&strings->host);
  full_url.Scheme().CopyToString(&strings->scheme);

  // LookupJoined, not Lookup1: a comma-split header is several map values,
  // which Lookup1 reports as absent.  For the capability headers
  // ("image/avif,image/webp,*/*", "gzip, deflate, br") the arms would
  // classify every such request as a no-capability client; for
  // Cache-Control ("no-cache, no-store") the forced reload would read as
  // not asked.  EVERY request-header read in this function is joined.
  strings->accept = request_headers.LookupJoined(HttpAttributes::kAccept);
  strings->user_agent =
      request_headers.LookupJoined(HttpAttributes::kUserAgent);
  strings->save_data = request_headers.LookupJoined("Save-Data");
  strings->accept_encoding =
      request_headers.LookupJoined(HttpAttributes::kAcceptEncoding);
  strings->if_none_match =
      request_headers.LookupJoined(HttpAttributes::kIfNoneMatch);
  strings->if_modified_since =
      request_headers.LookupJoined(HttpAttributes::kIfModifiedSince);

  out->url = strings->url;
  out->hostname = strings->host;
  out->scheme = strings->scheme;
  out->accept = strings->accept;
  out->user_agent = strings->user_agent;
  out->save_data = strings->save_data;
  out->accept_encoding = strings->accept_encoding;
  out->if_none_match = strings->if_none_match;
  out->if_modified_since = strings->if_modified_since;

  // A forced reload, in both of the spellings a client still sends, found
  // with a case-insensitive substring search over the joined value: the
  // directive is case-insensitive, and "no-cache, no-store" or a repeated
  // Pragma line must not read as absent.  Handed to the peer's freshness
  // evaluator rather than acted on here, as Apache does.
  GoogleString cache_control =
      request_headers.LookupJoined(HttpAttributes::kCacheControl);
  GoogleString pragma = request_headers.LookupJoined(HttpAttributes::kPragma);
  LowerString(&cache_control);
  LowerString(&pragma);
  out->force_revalidate =
      cache_control.find("no-cache") != GoogleString::npos ||
      pragma.find("no-cache") != GoogleString::npos;
}

// The names this request's server{} block is configured with, as copied
// when the configuration was loaded (NgxCollectConfiguredHostNames).  nginx's
// own server_names array is gone by the time a request runs, so this never
// touches it.  Empty for a server block without pagespeed options.
ConfiguredHostNames NgxConfiguredHostNames(ngx_http_request_t* r) {
  const ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  if (cfg_s == nullptr || cfg_s->host_names == nullptr) {
    return ConfiguredHostNames();
  }
  return *cfg_s->host_names;
}

// Answers an in-place request from the daemon's volume when it has one.
// Returns NGX_DECLINED when it did not answer -- the caller then falls
// through to the record marking, byte-for-byte as today.  Any other return
// is the send's own result, which ps_resource_handler returns to nginx.
//
// The reader is constructed, used and destroyed entirely inside this call,
// on the event thread: the volume close in the worker exit hook runs on the
// same thread after the record pool is shut down, so no reader outlives it
// and no factory call can be in flight at the close.  Never re-run the
// startup check on a live adapter.
ngx_int_t PsServeFromDaemonSubstrate(ngx_http_request_t* r,
                                     ps_srv_conf_t* cfg_s,
                                     const RequestHeaders& request_headers,
                                     const GoogleUrl& full_url) {
  // GET and HEAD only: a POST is never answered from the volume.  The
  // handler's own gates have already excluded subrequests; retested here so
  // the function is honest on its own.
  if (r != r->main ||
      (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD)) {
    return NGX_DECLINED;
  }

  NgxServerContext* server_context = cfg_s->server_context;
  // A null reader is ordinary: no log line, no alarm.  The skew class and
  // the fall-through counter are recorded exactly as Apache does, and the
  // record side posts the rate-limited open when its own factory returns
  // null -- nothing more is posted from here.
  std::unique_ptr<DaemonServeReader> reader(
      MakeDaemonServeReaderIfOpen(server_context->daemon_adapter()));
  DaemonServeStats* serve_stats = server_context->daemon_serve_stats();
  if (reader == nullptr) {
    if (serve_stats != nullptr) {
      serve_stats->Record(kPsServeClassOriginalSkew);
    }
    server_context->rewrite_stats()->ipro_daemon_fallthrough()->Add(1);
    return NGX_DECLINED;
  }

  // The operator's switch for the optimizer's stored compressed copies
  // (pagespeed DaemonServeStoredEncodings), per server block.  Off, the arm
  // selects and labels exactly as it always has.
  const NgxRewriteOptions* config = server_context->config();
  reader->set_serve_stored_encodings(config != nullptr &&
                                     config->daemon_serve_stored_encodings());
  // Lets the arm notice, while it serves a stored original, that the URL's
  // optimized copy has gone missing.  This worker process's own limiter:
  // one look per URL per window.  The look is one more read of the mapped
  // volume, like the selection it follows; the notification it can lead to
  // is posted below, never sent from this thread.
  reader->set_heal_notify_limiter(ProcessDaemonHealNotifyLimiter());

  PsDaemonServeRequestStrings strings;
  DaemonServeRequest serve_request;
  BuildDaemonServeRequest(request_headers, full_url, &strings, &serve_request);
  const DaemonServeDecision decision =
      reader->Serve(serve_request, server_context->timer()->NowMs() / 1000);
  // One serve class per consultation, including the fall-through below.
  if (serve_stats != nullptr) {
    serve_stats->Record(decision.serve_class);
  }
  if (decision.verdict == DaemonServeVerdict::kFallThrough) {
    server_context->rewrite_stats()->ipro_daemon_fallthrough()->Add(1);
    // An age-expired variant set must be purged before the re-record that
    // follows this fall-through, or the URL regresses to origin serving one
    // freshness lifetime after it was optimized (see
    // DaemonServeOriginRefreshedNotify's header).  Posted -- never inline
    // -- to the record arm's one-worker sequence, so it sits in that FIFO
    // ahead of this same request's record completion; the option context
    // is computed only now that the decision says there is something to
    // ask for, on this thread, the way the record arm computes it.  An
    // internal redirect can consult the arm twice and post the sentinel
    // twice; the worker rate-limits it per URL, so that is duplicate
    // traffic, not a defect.
    if (decision.stale_variant_expired_by_age) {
      DaemonAdapter* adapter = server_context->daemon_adapter();
      const SystemRewriteOptions* options =
          SystemRewriteOptions::DynamicCast(server_context->global_options());
      GoogleString option_context, option_signature;
      if (adapter != nullptr && adapter->abi() != nullptr &&
          options != nullptr &&
          OptionContext::Compute(*options, &option_context,
                                 &option_signature) ==
              OptionContextStatus::kOk) {
        RewriteStats* stats = server_context->rewrite_stats();
        PostDaemonServeNotify(
            g_daemon_record_sequence, DaemonServeNotifyKind::kOriginRefreshed,
            adapter->abi(), adapter->socket_path(), serve_request, decision,
            option_context, option_signature,
            stats->ipro_daemon_refresh_notified(),
            stats->ipro_daemon_refresh_notify_failed(),
            server_context->statistics()->FindVariable(
                kIproDaemonNotifyDropped));
      }
    }
    return NGX_DECLINED;
  }

  // The one read of the body's length, captured while the owner of the
  // borrow is alive: every later use -- the composed Content-Length, the
  // pool copy below, the hit record after the reader is gone -- takes this
  // local, never the StringPiece again.
  const size_t body_size = decision.body.size();

  // The media type the response carries: the decision's own when it has
  // one, else nginx's type-map answer for the URI -- asked on BOTH legs,
  // because the answer feeds the Vary compressibility predicate, and the
  // predicate's input must not depend on the leg.  The HEADER stays
  // leg-gated: on the 304 what the type-map call put on r->headers_out is
  // cleared again (value, length, lowcase pointer), so the 304 still
  // carries no Content-Type.  (The type-map call may also lowercase
  // r->exten in place; nothing downstream reads it.)
  GoogleString request_media_type;
  if (decision.content_type.empty() && ngx_http_set_content_type(r) == NGX_OK &&
      r->headers_out.content_type.len > 0) {
    request_media_type.assign(
        reinterpret_cast<char*>(r->headers_out.content_type.data),
        r->headers_out.content_type.len);
    if (decision.not_modified) {
      // Read for the composition only; a 304 invents no Content-Type.
      ngx_str_null(&r->headers_out.content_type);
      r->headers_out.content_type_len = 0;
      r->headers_out.content_type_lowcase = nullptr;
    }
  }

  // Compressible for this server = named by ContentType::IsCompressible()
  // or present in the type list this module's own gzip setter installs
  // (application/pdf, application/postscript, text/csv close that gap);
  // asked about the SERVED type, the same one the compressor would see.
  const char* served_media_type = ServeCompressorMediaType(
      decision,
      request_media_type.empty() ? nullptr : request_media_type.c_str());
  const bool server_compresses_type =
      NgxGZipSetterCompressesType(served_media_type);

  // The composer writes everything into response_headers; the value it
  // returns exists for its unit tests and is not needed here.
  ResponseHeaders response_headers;
  PsComposeDaemonServeHeaders(decision, request_media_type, body_size,
                              server_compresses_type, &response_headers);
  // Vary is the both-legs composition: the arm states the encoding token on
  // the 200 AND the 304 for a compressible served type, with no size floor,
  // because nginx's gzip eligibility (its floor, its type list, the
  // operator's switches) cannot be read from here -- the core gzip filter
  // decides after this module has composed its headers.  The two legs agree
  // and nothing an operator configures can narrow them.  The arm's line is
  // the only one on the wire: copying it into headers_out below marks the
  // request, and the module's filter that runs after the compressor then
  // withdraws the compressor's own `Vary` stamp.

  ngx_buf_t* body_buf = nullptr;
  ngx_chain_t out;
  out.buf = nullptr;
  out.next = nullptr;
  if (!decision.not_modified) {
    // The body, copied into ONE request-pool buffer before the reader that
    // owns the borrow is released.  One buffer, not a chunked chain: nginx's
    // range body filter slices in-memory buffers, and a multipart range
    // across buffers is an error, so the response must be sliceable -- the
    // copy count is still exactly one.  A HEAD copies nothing: the method
    // has been known since the request line, its body is never sent, and
    // the Content-Length above still states the length.
    const bool send_body = (r->method & NGX_HTTP_HEAD) == 0;
    u_char* body_copy = nullptr;
    if (send_body && body_size > 0) {
      body_copy = static_cast<u_char*>(ngx_palloc(r->pool, body_size));
      if (body_copy == nullptr) {
        return NGX_ERROR;
      }
      ngx_memcpy(body_copy, decision.body.data(), body_size);
    }
    body_buf = static_cast<ngx_buf_t*>(ngx_calloc_buf(r->pool));
    if (body_buf == nullptr) {
      return NGX_ERROR;
    }
    body_buf->pos = body_copy;
    body_buf->last = body_copy == nullptr ? nullptr : body_copy + body_size;
    // No bytes in memory is a sync buffer, exactly as
    // string_piece_to_buffer_chain marks it: a non-special zero-size buffer
    // trips ngx_http_write_filter's "zero size buf" alert.
    if (body_copy == nullptr) {
      body_buf->sync = 1;
    } else {
      body_buf->temporary = 1;
    }
    body_buf->last_buf = 1;
    out.buf = body_buf;
  }

  // The reader -- and the peer's read result the body was borrowed from --
  // is released here, before the first byte reaches the socket.
  reader.reset();

  if (copy_response_headers_to_ngx(r, response_headers, kDontPreserveHeaders) !=
      NGX_OK) {
    return NGX_ERROR;
  }
  if (!decision.not_modified) {
    // Range support is nginx's: with a real Content-Length and allow_ranges
    // set, the range filter slices the buffer and emits Accept-Ranges.
    r->allow_ranges = 1;
    // The arm has answered the conditionals by its own rules (a literal "*"
    // is deliberately not honoured); nginx's own not-modified filter runs
    // AHEAD OF this module's and sees this response first, and it must not
    // mint a second opinion for If-None-Match: *, If-Match or
    // If-Unmodified-Since on top of the arm's.
    r->disable_not_modified = 1;
  }

  // Statistics exactly as Apache sets them: the serve counted once for
  // every answered request -- 200, 304 and HEAD alike.  It sits BEFORE the
  // send because the send's early return must not skip it; reachable only
  // after ngx_http_output_filter is exactly where it must not be.  The hit
  // is recorded under Apache's exact condition -- optimized, worker
  // processed, origin length known -- on the 200 leg only, never on a 304.
  server_context->rewrite_stats()->ipro_daemon_served()->Add(1);
  RecordDaemonServedClass(server_context->rewrite_stats(),
                          decision.ps_content_type);
  if (serve_stats != nullptr && !decision.not_modified &&
      decision.serve_class == kPsServeClassOptimized &&
      decision.worker_processed && decision.origin_content_length > 0) {
    // Attributed to a name this server block lists, never the request's
    // Host value on its own; the entry was looked up for the request's host.
    serve_stats->RecordHit(
        decision.ps_content_type, decision.origin_content_length, body_size,
        decision.stored_mask,
        VouchedServeHost(serve_request.hostname, NgxConfiguredHostNames(r)));
  }
  // A fallback hit is SERVED, and the variant the client's mask really
  // names is asked for with one fire-and-forget notification -- posted to
  // the record arm's one-worker sequence, never inline on the event
  // thread.  It sits where the served counter sits, ahead of the send: a
  // fallback hit answered as a 304 or to a HEAD asks too (as Apache's
  // unconditional block does), or a client holding the fallback bytes would
  // revalidate for ever without ever asking for its own variant.  The
  // option context is computed only now that the decision says fallback: an
  // exact serve, the converged steady state, never pays it.  The two
  // suppressions (the 0x04 gate, an unnameable context) stay inside
  // DaemonServeFallbackRenotify, and a request whose configuration cannot
  // be named asks for nothing, mirroring the record gate.
  if (decision.fallback_hit) {
    DaemonAdapter* adapter = server_context->daemon_adapter();
    const SystemRewriteOptions* options =
        SystemRewriteOptions::DynamicCast(server_context->global_options());
    GoogleString option_context, option_signature;
    if (adapter != nullptr && adapter->abi() != nullptr && options != nullptr &&
        OptionContext::Compute(*options, &option_context, &option_signature) ==
            OptionContextStatus::kOk) {
      RewriteStats* stats = server_context->rewrite_stats();
      PostDaemonServeNotify(
          g_daemon_record_sequence, DaemonServeNotifyKind::kFallbackRenotify,
          adapter->abi(), adapter->socket_path(), serve_request, decision,
          option_context, option_signature,
          stats->ipro_daemon_fallback_notified(),
          stats->ipro_daemon_fallback_notify_failed(),
          server_context->statistics()->FindVariable(kIproDaemonNotifyDropped));
    }
  }
  // A stored original served for a URL whose optimized copy went missing is
  // asked for with one fire-and-forget notification, posted like the
  // fallback re-notify above and for the same reason placed ahead of the
  // send: a 304 or a HEAD answered from the stored original asks too.  The
  // arm's limiter has already held it to about once per URL per window, and
  // its detection never fires for content the optimizer left unoptimized
  // on purpose.  The response is not changed by this.
  if (decision.lost_optimized_copy) {
    DaemonAdapter* adapter = server_context->daemon_adapter();
    const SystemRewriteOptions* options =
        SystemRewriteOptions::DynamicCast(server_context->global_options());
    GoogleString option_context, option_signature;
    if (adapter != nullptr && adapter->abi() != nullptr && options != nullptr &&
        OptionContext::Compute(*options, &option_context, &option_signature) ==
            OptionContextStatus::kOk) {
      RewriteStats* stats = server_context->rewrite_stats();
      PostDaemonServeNotify(
          g_daemon_record_sequence, DaemonServeNotifyKind::kLostCopy,
          adapter->abi(), adapter->socket_path(), serve_request, decision,
          option_context, option_signature, stats->ipro_daemon_heal_notified(),
          stats->ipro_daemon_heal_notify_failed(),
          server_context->statistics()->FindVariable(kIproDaemonNotifyDropped));
    }
  }
  ngx_int_t rc = ngx_http_send_header(r);
  if (rc == NGX_ERROR || rc > NGX_OK || r->header_only ||
      decision.not_modified) {
    // The 304 is headers only by design (the emit attached no body), and a
    // HEAD skips the body through the same check send_out_headers_and_body
    // makes.
    return rc;
  }
  rc = ngx_http_output_filter(r, &out);
  return rc;
}

}  // namespace

ngx_int_t ps_resource_handler(ngx_http_request_t* r, bool html_rewrite,
                              RequestRouting::Response response_category) {
  if (r != r->main) {
    return NGX_DECLINED;
  }

  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  ps_request_ctx_t* ctx = ps_get_request_context(r);

  if (ctx != nullptr) {
    // A request that comes through here again (internal redirect, or the
    // html pass after the resource pass) starts the disposition over: a
    // pending mark from a previous pass must not leak into this one's
    // filters when this pass takes no record branch.
    ctx->daemon_record_pending = false;
  }

  if (ngx_terminate || ngx_exiting) {
    cfg_s->server_context->message_handler()->Message(
        kInfo, "ps_resource_handler declining: nginx worker is shutting down");

    if (ctx == nullptr) {
      return NGX_DECLINED;
    }
    ps_release_base_fetch(ctx);
    return NGX_DECLINED;
  }

  CHECK(!(html_rewrite && (ctx == nullptr || ctx->html_rewrite == false)));

  if (!html_rewrite && r->method != NGX_HTTP_GET &&
      r->method != NGX_HTTP_HEAD && r->method != NGX_HTTP_POST &&
      response_category != RequestRouting::kCachePurge) {
    return NGX_DECLINED;
  }

  GoogleString url_string = ps_determine_url(r);
  GoogleUrl url(url_string);

  if (!url.IsWebValid()) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "invalid url");
    return NGX_DECLINED;
  }

  std::unique_ptr<RequestHeaders> request_headers =
      std::make_unique<RequestHeaders>();
  std::unique_ptr<ResponseHeaders> response_headers =
      std::make_unique<ResponseHeaders>();

  copy_request_headers_from_ngx(r, request_headers.get());
  copy_response_headers_from_ngx(r, response_headers.get());

  RequestContextPtr request_context(
      cfg_s->server_context->NewRequestContext(r));
  GoogleString pagespeed_query_params;
  GoogleString pagespeed_option_cookies;
  RewriteOptions* options = ps_determine_remote_options(cfg_s);
  if (!ps_determine_options(r, request_headers.get(), response_headers.get(),
                            &options, request_context, cfg_s, &url,
                            &pagespeed_query_params, &pagespeed_option_cookies,
                            html_rewrite)) {
    return NGX_ERROR;
  }

  // Take ownership of custom_options.
  std::unique_ptr<RewriteOptions> custom_options(options);
  if (options == nullptr) {
    options = cfg_s->server_context->global_options();
  }

  request_context->set_options(options->ComputeHttpOptions());

  // ps_determine_options modified url, removing any ModPagespeedFoo=Bar query
  // parameters.  Keep url_string in sync with url.
  url.Spec().CopyToString(&url_string);

  if (cfg_s->server_context->global_options()->respect_x_forwarded_proto()) {
    bool modified_url = ps_apply_x_forwarded_proto(r, &url_string);
    if (modified_url) {
      url.Reset(url_string);
      CHECK(url.IsWebValid()) << "The output of ps_apply_x_forwarded_proto"
                              << " should always be a valid url because it only"
                              << " changes the scheme between http and https.";
    }
  }

  bool pagespeed_resource =
      !html_rewrite && cfg_s->server_context->IsPagespeedResource(url);
  bool is_an_admin_handler =
      response_category == RequestRouting::kStatistics ||
      response_category == RequestRouting::kGlobalStatistics ||
      response_category == RequestRouting::kConsole ||
      response_category == RequestRouting::kAdmin ||
      response_category == RequestRouting::kGlobalAdmin ||
      response_category == RequestRouting::kCachePurge;

  // Normally if we're disabled we won't handle any requests, but if we're in
  // standby mode we do want to handle requests for .pagespeed. resources.
  if (options->unplugged() || (!options->enabled() && !pagespeed_resource)) {
    // Disabled via query params or request headers.
    return NGX_DECLINED;
  }

  if (!html_rewrite) {
    // create request ctx
    CHECK(ctx == nullptr);
    ctx = new ps_request_ctx_t();

    ctx->r = r;
    ctx->html_rewrite = false;
    ctx->in_place = false;
    ctx->follow_flushes = options->follow_flushes();
    ctx->preserve_caching_headers = kDontPreserveHeaders;

    // See build_context_for_request() in mod_instaweb.cc
    // TODO(jefftk): Is this the right place to be modifying caching headers for
    // html fetches?  Or should that be done later, in the headers flow for
    // filter mode, rather than here in resource fetch mode?
    if (!options->modify_caching_headers()) {
      ctx->preserve_caching_headers = kPreserveAllCachingHeaders;
    } else if (!options->IsDownstreamCacheIntegrationEnabled()) {
      // Downstream cache integration is not enabled. Disable original
      // Cache-Control headers.
      ctx->preserve_caching_headers = kDontPreserveHeaders;
    } else if (!pagespeed_resource && !is_an_admin_handler) {
      ctx->preserve_caching_headers = kPreserveOnlyCacheControl;
      // Downstream cache integration is enabled. If a rebeaconing key has been
      // configured and there is a ShouldBeacon header with the correct key,
      // disable original Cache-Control headers so that the instrumented page is
      // served out with no-cache.
      StringPiece should_beacon(request_headers->Lookup1(kPsaShouldBeacon));
      if (options->MatchesDownstreamCacheRebeaconingKey(should_beacon)) {
        ctx->preserve_caching_headers = kDontPreserveHeaders;
      }
    }

    ctx->recorder = nullptr;
    ctx->daemon_record_pending = false;
    ctx->daemon_recorder = false;
    ctx->daemon_body_bytes = 0;
    ctx->url_string = url_string;
    ctx->location_field_set = false;
    ctx->psol_vary_accept_only = false;

    // Set up a cleanup handler on the request.
    ngx_http_cleanup_t* cleanup = ngx_http_cleanup_add(r, 0);
    if (cleanup == nullptr) {
      ps_release_request_context(ctx);
      return NGX_ERROR;
    }
    cleanup->handler = ps_release_request_context;
    cleanup->data = ctx;
    ngx_http_set_ctx(r, ctx, ngx_pagespeed);
  }

  if (pagespeed_resource) {
    // TODO(jefftk): Set using_spdy appropriately.  See
    // ProxyInterface::ProxyRequestCallback
    ps_create_base_fetch(url.Spec(), ctx, request_context,
                         request_headers.release(), kPageSpeedResource,
                         options);
    ResourceFetch::Start(
        url, custom_options.release() /* null if there aren't custom options */,
        cfg_s->server_context, ctx->base_fetch);
    return ps_async_wait_response(r);
  } else if (is_an_admin_handler) {
    // Defense-in-depth: log one warning per handler family per process
    // lifetime if a non-loopback client reaches an admin endpoint. The
    // deploying admin's nginx config (allow 127.0.0.1; deny all;) is the
    // real gate. See pagespeed/system/admin_site.h.
    ps_warn_admin_exposure(r, cfg_s->server_context, response_category);

    ps_create_base_fetch(url.Spec(), ctx, request_context,
                         request_headers.release(), kAdminPage, options);
    QueryParams query_params;
    query_params.ParseFromUrl(url);

    PosixTimer timer;
    int64 now_ms = timer.NowMs();
    ctx->base_fetch->response_headers()->SetDateAndCaching(
        now_ms, 0 /* max-age */, ", no-cache");

    if (response_category == RequestRouting::kStatistics ||
        response_category == RequestRouting::kGlobalStatistics) {
      cfg_s->server_context->StatisticsPage(
          response_category == RequestRouting::kGlobalStatistics, query_params,
          cfg_s->server_context->config(), ctx->base_fetch);
    } else if (response_category == RequestRouting::kConsole) {
      cfg_s->server_context->ConsoleHandler(*cfg_s->server_context->config(),
                                            AdminSite::kStatistics,
                                            query_params, ctx->base_fetch);
    } else if (response_category == RequestRouting::kAdmin ||
               response_category == RequestRouting::kGlobalAdmin) {
      // For POST requests, the body was read by ps_admin_body_handler
      // before we got here.
      StringPiece request_body;
      if (r->method == NGX_HTTP_POST && r->request_body != nullptr) {
        ps_request_body_to_string_piece(r, &request_body);
        if (request_body.size() > ADMIN_MAX_POST_SIZE) {
          ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                        "admin POST body too large: %uz > %uz",
                        request_body.size(),
                        static_cast<size_t>(ADMIN_MAX_POST_SIZE));
          return NGX_HTTP_REQUEST_ENTITY_TOO_LARGE;
        }
      }
      ctx->base_fetch->request_headers()->set_method(
          RequestHeaders::MethodFromString(
              str_to_string_piece(r->method_name)));
      // The host this server block records its serves under (the same rule
      // as the serve hit): a per-host console sees that row of the
      // optimizer's serve savings and no other site's.
      cfg_s->server_context->AdminPage(
          response_category == RequestRouting::kGlobalAdmin, url, query_params,
          custom_options == nullptr ? cfg_s->server_context->config()
                                    : custom_options.get(),
          ctx->base_fetch, request_body,
          VouchedServeHost(url.Host(), NgxConfiguredHostNames(r)));
    } else if (response_category == RequestRouting::kCachePurge) {
      AdminSite* admin_site = cfg_s->server_context->admin_site();
      admin_site->PurgeHandler(url_string, cfg_s->server_context->cache_path(),
                               ctx->base_fetch);
    } else {
      CHECK(false);
    }

    return ps_async_wait_response(r);
  } else if (!html_rewrite && response_category == RequestRouting::kResource) {
    bool is_proxy = false;
    GoogleString mapped_url;
    GoogleString host_header;

    if (options->domain_lawyer()->MapOriginUrl(url, &mapped_url, &host_header,
                                               &is_proxy) &&
        is_proxy) {
      ps_create_base_fetch(url.Spec(), ctx, request_context,
                           request_headers.release(), kPageSpeedProxy, options);

      RewriteDriver* driver;
      if (custom_options.get() == nullptr) {
        driver = cfg_s->server_context->NewRewriteDriver(
            ctx->base_fetch->request_context());
      } else {
        driver = cfg_s->server_context->NewCustomRewriteDriver(
            custom_options.release(), ctx->base_fetch->request_context());
      }

      driver->SetRequestHeaders(*ctx->base_fetch->request_headers());
      ps_apply_webbotauth_verdict(r, cfg_s, driver);
      driver->set_pagespeed_query_params(pagespeed_query_params);
      driver->set_pagespeed_option_cookies(pagespeed_option_cookies);
      cfg_s->proxy_fetch_factory->StartNewProxyFetch(
          mapped_url, ctx->base_fetch, driver, nullptr /*property_callback*/,
          nullptr /*original_content_fetch*/);

      return ps_async_wait_response(r);
    }
  }

  if (html_rewrite && options->IsAllowed(url.Spec())) {
    ps_create_base_fetch(url.Spec(), ctx, request_context,
                         request_headers.release(), kHtmlTransform, options);
    // Do not store driver in request_context, it's not safe.
    RewriteDriver* driver;

    // If we don't have custom options we can use NewRewriteDriver which reuses
    // rewrite drivers and so is faster because there's no wait to construct
    // them.  Otherwise we have to build a new one every time.

    if (custom_options.get() == nullptr) {
      driver = cfg_s->server_context->NewRewriteDriver(
          ctx->base_fetch->request_context());
    } else {
      // NewCustomRewriteDriver takes ownership of custom_options.
      driver = cfg_s->server_context->NewCustomRewriteDriver(
          custom_options.release(), ctx->base_fetch->request_context());
    }

    driver->SetRequestHeaders(*ctx->base_fetch->request_headers());
    ps_apply_webbotauth_verdict(r, cfg_s, driver);
    driver->set_pagespeed_query_params(pagespeed_query_params);
    driver->set_pagespeed_option_cookies(pagespeed_option_cookies);

    // TODO(jefftk): FlushEarlyFlow would go here.
    ProxyFetchPropertyCallbackCollector* property_callback =
        ProxyFetchFactory::InitiatePropertyCacheLookup(
            !html_rewrite /* is_resource_fetch */, url, cfg_s->server_context,
            options, ctx->base_fetch);

    // Will call StartParse etc.  The rewrite driver will take care of deleting
    // itself if necessary.
    ctx->proxy_fetch = cfg_s->proxy_fetch_factory->CreateNewProxyFetch(
        url_string, ctx->base_fetch, driver, property_callback,
        nullptr /* original_content_fetch */);
    ctx->proxy_fetch->set_trusted_input(true);
    return NGX_OK;
  }

  // In-place optimization considers GET and HEAD only: those are the
  // methods whose responses may be looked up in, and recorded into, the
  // cache.  Any other method takes the path a request takes when in-place
  // rewriting is disabled: no lookup, no recorder mark, and the NGX_DECLINED
  // below lets nginx answer it itself (a POST to a static file gets the
  // static handler's 405).  nginx's method constants are single-bit flags,
  // so the & below tests membership of the one method the request has.
  //
  // In-place optimization also leaves internally redirected requests to the
  // server: it does not look them up and does not record them (r->internal
  // is set on the pass after an internal redirect).
  if (options->in_place_rewriting_enabled() && options->enabled() &&
      options->IsAllowed(url.Spec()) &&
      (r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD)) && !r->internal) {
    const IproDisposition disposition =
        IproDispositionFor(cfg_s->server_context->daemon_health());
    if (disposition == IproDisposition::kDaemonSubstrate) {
      // The daemon owns the in-place cache: FIRST ask its volume for a
      // response.  A served request is answered here and never reaches the
      // tail below; a declined request falls through to exactly what
      // happens today -- the in-place header filter builds the recorder on
      // the mark set here, and a served request never carries it.
      // Subrequests never reach here (declined above).
      const ngx_int_t serve_rc =
          PsServeFromDaemonSubstrate(r, cfg_s, *request_headers, url);
      if (serve_rc != NGX_DECLINED) {
        return serve_rc;
      }
      //
      // The method is the HEAD guard, not r->header_only: nginx sets
      // header_only in its core header filter, downstream of this module's,
      // so at the module's own position the flag is not set for a HEAD yet.
      // The method has been known since the request line was parsed, and a
      // HEAD has no body to record.
      if (!(r->method & NGX_HTTP_HEAD)) {
        ctx->daemon_record_pending = true;
      }
    } else if (disposition == IproDisposition::kOff) {
      // A daemon was configured and is not usable: in-place optimization is
      // off, exactly as if in_place_rewriting were disabled.
    } else {
      ps_create_base_fetch(url.Spec(), ctx, request_context,
                           request_headers.release(), kIproLookup, options);

      // Do not store driver in request_context, it's not safe.
      RewriteDriver* driver;
      if (custom_options.get() == nullptr) {
        driver = cfg_s->server_context->NewRewriteDriver(
            ctx->base_fetch->request_context());
      } else {
        // NewCustomRewriteDriver takes ownership of custom_options.
        driver = cfg_s->server_context->NewCustomRewriteDriver(
            custom_options.release(), ctx->base_fetch->request_context());
      }

      driver->SetRequestHeaders(*ctx->base_fetch->request_headers());
      ps_apply_webbotauth_verdict(r, cfg_s, driver);
      ctx->driver = driver;

      cfg_s->server_context->message_handler()->Message(
          kInfo, "Trying to serve rewritten resource in-place: %s",
          url_string.c_str());

      ctx->in_place = true;
      ctx->driver->FetchInPlaceResource(url, false /* proxy_mode */,
                                        ctx->base_fetch);

      return ps_async_wait_response(r);
    }
  }

  // NOTE: We are using the below debug message as is for some of our system
  // tests. So, be careful about test breakages caused by changing or
  // removing this line.
  ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "Passing on content handling for non-pagespeed resource '%s'",
                url_string.c_str());
  CHECK(ctx->base_fetch == nullptr);
  // set html_rewrite flag.
  ctx->html_rewrite = true;
  return NGX_DECLINED;
}

// Send each buffer in the chain to the proxy_fetch for optimization.
// Eventually it will make it's way, optimized, to base_fetch.
void ps_send_to_pagespeed(ngx_http_request_t* r, ps_request_ctx_t* ctx,
                          ps_srv_conf_t* cfg_s, ngx_chain_t* in) {
  ngx_chain_t* cur;
  int last_buf = 0;
  size_t total_bytes = 0;
  int buffer_count = 0;
  for (cur = in; cur != nullptr; cur = cur->next) {
    last_buf = cur->buf->last_buf;
    total_bytes += (cur->buf->last - cur->buf->pos);
    buffer_count++;
    // Buffers are not really the last buffer until they've been through
    // pagespeed.
    cur->buf->last_buf = 0;

    CHECK(ctx->proxy_fetch != nullptr);
    if (ctx->inflater_ == nullptr) {
      ctx->proxy_fetch->Write(
          StringPiece(reinterpret_cast<char*>(cur->buf->pos),
                      cur->buf->last - cur->buf->pos),
          cfg_s->handler);
    } else {
      char buf[kStackBufferSize];
      ctx->inflater_->SetInput(reinterpret_cast<char*>(cur->buf->pos),
                               cur->buf->last - cur->buf->pos);
      while (ctx->inflater_->HasUnconsumedInput()) {
        int num_inflated_bytes =
            ctx->inflater_->InflateBytes(buf, kStackBufferSize);
        if (num_inflated_bytes < 0) {
          cfg_s->handler->Message(kWarning, "Corrupted inflation");
        } else if (num_inflated_bytes > 0) {
          ctx->proxy_fetch->Write(StringPiece(buf, num_inflated_bytes),
                                  cfg_s->handler);
        }
      }
    }
    if (cur->buf->flush && ctx->follow_flushes) {
      // Calling ctx->proxy_fetch->Flush(cfg_s->handler) will be a no-op here,
      // unless we have follow_flushes or flush_html enabled. Note that PSOL
      // might aggregate multiple flushes into 1, and actually flush a little bit
      // later due to html parser state and earlier scheduled operations.
      // Also, unless we also set the flush flag on the nginx buffers we won't
      // actually flush.
      // Note that too many flushes could harm optimization over larger html
      // fragments as PSOL gets less context to work with, e.g. it can't combine
      // two css files if a flush happens in between.
      ctx->proxy_fetch->Flush(cfg_s->handler);
    }

    // We're done with buffers as we pass them through, so mark them as sent as
    // we go.
    cur->buf->pos = cur->buf->last;
  }

  ngx_log_debug3(
      NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "pagespeed ps_send_to_pagespeed: %d buffers, %uz bytes, last_buf=%d",
      buffer_count, total_bytes, last_buf);

  if (last_buf) {
    ctx->proxy_fetch->Done(true /* success */);
    ctx->proxy_fetch = nullptr;  // ProxyFetch deletes itself on Done().
  }
}

#ifndef ngx_http_clear_etag
// The ngx_http_clear_etag(r) macro was added in 1.3.3.  Backport it if it's not
// present.
#define ngx_http_clear_etag(r)     \
  if (r->headers_out.etag) {       \
    r->headers_out.etag->hash = 0; \
    r->headers_out.etag = NULL;    \
  }
#endif

void ps_strip_html_headers(ngx_http_request_t* r) {
  // We're modifying content, so switch to 'Transfer-Encoding: chunked' and
  // calculate on the fly.
  ngx_http_clear_content_length(r);

  ngx_table_elt_t* header;
  NgxListIterator it(&(r->headers_out.headers.part));
  while ((header = it.Next()) != nullptr) {
    // We also need to strip:
    //   Accept-Ranges
    //    - won't work because our html changes
    //   Vary: Accept-Encoding
    //    - our gzip filter will add this later
    if (STR_CASE_EQ_LITERAL(header->key, "Accept-Ranges") ||
        (STR_CASE_EQ_LITERAL(header->key, "Vary") &&
         STR_CASE_EQ_LITERAL(header->value, "Accept-Encoding"))) {
      // Response headers with hash of 0 are excluded from the response.
      header->hash = 0;
    }
  }
}

// Returns true, if the the response headers indicate there are multiple
// content encodings.
bool ps_has_stacked_content_encoding(ngx_http_request_t* r) {
  ngx_uint_t i;
  ngx_list_part_t* part = &(r->headers_out.headers.part);
  ngx_table_elt_t* header = static_cast<ngx_table_elt_t*>(part->elts);
  int field_count = 0;

  for (i = 0; /* void */; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }

      part = part->next;
      header = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }

    // Inspect Content-Encoding headers, checking all value fields
    // If an origin returns gzip,foo, that is what we will get here.
    if (STR_CASE_EQ_LITERAL(header[i].key, "Content-Encoding")) {
      if (header[i].value.data != nullptr && header[i].value.len > 0) {
        char* p = reinterpret_cast<char*>(header[i].value.data);
        ngx_uint_t j;
        for (j = 0; j < header[i].value.len; j++) {
          if (p[j] == ',' || j == header[i].value.len - 1) {
            field_count++;
          }
        }
        if (field_count > 1) {
          return true;
        }
      }
    }
  }

  return false;
}

ngx_int_t ps_etag_header_filter(ngx_http_request_t* r) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);
#if (NGX_HTTP_GZIP)
  if (ctx && ctx->psol_vary_accept_only) {
    // One shot, and only while the module's Vary is really on this
    // response: a response that replaced the module's (an error page sent
    // after the headers were cleaned) keeps the compressor's own stamp.
    ctx->psol_vary_accept_only = false;
    ngx_table_elt_t* header;
    NgxListIterator it(&(r->headers_out.headers.part));
    while ((header = it.Next()) != nullptr) {
      if (header->hash != 0 && STR_CASE_EQ_LITERAL(header->key, "Vary") &&
          ps_vary_lists_accept_encoding(header->value)) {
        r->gzip_vary = 0;
        break;
      }
    }
  }
#endif

  // The s-maxage rewrite is a classic-substrate mechanism: it shortens a
  // downstream cache's hold on UNOPTIMIZED bytes served from this module's
  // own cache so they are re-fetched once optimized.  A daemon-side recorder
  // changes nothing about how this response is served -- it is the origin's
  // answer passing through -- so its Cache-Control stays the origin's.
  //
  // The daemon conjunct is LOAD-BEARING: with the module in its proper chain
  // position this filter runs after the in-place header filter, so on the
  // daemon path the recorder already exists when this gate is evaluated, and
  // the conjunct is the only thing that keeps the origin's Cache-Control
  // from being rewritten here.
  if (ctx && ctx->recorder && !ctx->daemon_recorder) {
    ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
    int s_maxage_sec =
        cfg_s->server_context->global_options()->EffectiveInPlaceSMaxAgeSec();
    if (s_maxage_sec != -1) {
      GoogleString existing_cache_control;
      bool cache_control_present =
          ps_get_cache_control(r, &existing_cache_control);
      GoogleString updated_cache_control;
      if (ResponseHeaders::ApplySMaxAge(s_maxage_sec, existing_cache_control,
                                        &updated_cache_control)) {
        // We're modifing the cache control header; save a copy first.
        // NULL indicates that the header was not present.
        ctx->recorder->SaveCacheControl(
            cache_control_present ? existing_cache_control.c_str() : nullptr);

        // Replace the cache-control with our new s-maxage-including one.
        ps_set_cache_control(
            r, string_piece_to_pool_string(r->pool, updated_cache_control));
      }
    }
  }

  return ngx_http_ef_next_header_filter(r);
}

// Makes sure a body buffer's bytes are in memory.  A defensive fallback: see
// the two cases inside.  Returns NGX_OK on success, NGX_ERROR on failure.
static ngx_int_t ps_read_file_buffer(ngx_http_request_t* r, ngx_buf_t* buf) {
  // With sendfile on, nginx's copy filter hands on a buffer that is in memory
  // AND still marked as file-backed (so an untouched buffer can go out through
  // sendfile).  Such a buffer needs no read: its bytes are already at pos.
  if (!buf->in_file || ngx_buf_in_memory(buf)) {
    return NGX_OK;  // Already in memory
  }

  off_t size = buf->file_last - buf->file_pos;
  if (size <= 0) {
    return NGX_OK;  // Empty file buffer
  }

  // A buffer that is ONLY file-backed.  With the module in its proper chain
  // position the copy filter has read static files into memory before the
  // body filters run, so this is not expected; if some flow still hands over
  // a pure file buffer, the read below happens synchronously on the event
  // thread, and it must stay observable, not silent.
  ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                "pagespeed: synchronous read of %O bytes for %V", size,
                &r->uri);

  // Allocate memory for the file content
  u_char* data = static_cast<u_char*>(ngx_palloc(r->pool, size));
  if (data == nullptr) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "pagespeed: failed to allocate %O bytes for file buffer",
                  size);
    return NGX_ERROR;
  }

  // Read the file content
  ssize_t n = ngx_read_file(buf->file, data, size, buf->file_pos);
  if (n == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                  "pagespeed: failed to read file buffer");
    return NGX_ERROR;
  }

  if (n != size) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "pagespeed: partial read of file buffer: %z of %O", n, size);
    return NGX_ERROR;
  }

  // Convert the buffer from file-based to memory-based
  buf->pos = data;
  buf->last = data + size;
  buf->start = data;
  buf->end = data + size;
  buf->in_file = 0;
  buf->file_pos = 0;
  buf->file_last = 0;
  buf->file = nullptr;
  buf->memory = 1;
  buf->temporary = 1;

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed: read %O bytes from file buffer into memory", size);

  return NGX_OK;
}

namespace html_rewrite {
ngx_http_output_header_filter_pt ngx_http_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_next_body_filter;

// After pagespeed has had a chance to run, copy the headers it produced to
// nginx so it can send them out to the browser.
ngx_int_t ps_html_rewrite_header_filter(ngx_http_request_t* r) {
  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  if (ps_disabled(cfg_s)) {
    return ngx_http_next_header_filter(r);
  }

  if (r != r->main) {
    // Don't handle subrequests.
    return ngx_http_next_header_filter(r);
  }
  // Poll for cache flush on every request (polls are rate-limited).
  cfg_s->server_context->FlushCacheIfNecessary();

  ps_request_ctx_t* ctx = ps_get_request_context(r);

  if (ctx == nullptr || ctx->html_rewrite == false) {
    return ngx_http_next_header_filter(r);
  }

  if (r->err_status != 0) {
    ctx->html_rewrite = false;
    return ngx_http_next_header_filter(r);
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "http pagespeed html rewrite header filter \"%V\"", &r->uri);

  // We don't know what this request is, but we only want to send html through
  // to pagespeed.  Check the content type header and find out.
  const ContentType* content_type =
      MimeTypeToContentType(str_to_string_piece(r->headers_out.content_type));
  if (content_type == nullptr || !content_type->IsHtmlLike()) {
    // Unknown or otherwise non-html content type: skip it.
    ctx->html_rewrite = false;
    return ngx_http_next_header_filter(r);
  }

  ngx_int_t rc = ps_resource_handler(r, true /* html rewrite */,
                                     RequestRouting::kResource);
  if (rc != NGX_OK) {
    ctx->html_rewrite = false;
    return ngx_http_next_header_filter(r);
  }

  if (r->headers_out.content_encoding &&
      r->headers_out.content_encoding->value.len) {
    // headers_out.content_encoding will be set to the exact last
    // Content-Encoding response header value that nginx receives. To
    // check if there were multiple (aka stacked) encodings in the
    // response headers, we must iterate them all.
    if (!ps_has_stacked_content_encoding(r)) {
      StringPiece content_encoding =
          str_to_string_piece(r->headers_out.content_encoding->value);
      GzipInflater::InflateType inflate_type = GzipInflater::kGzip;
      bool is_encoded = false;
      if (StringCaseEqual(content_encoding, "deflate")) {
        is_encoded = true;
        inflate_type = GzipInflater::kDeflate;
      } else if (StringCaseEqual(content_encoding, "gzip")) {
        is_encoded = true;
      }

      if (is_encoded) {
        r->headers_out.content_encoding->hash = 0;
        r->headers_out.content_encoding = nullptr;
        ctx->inflater_ = new GzipInflater(inflate_type);
        ctx->inflater_->Init();
      }
    }
  }

  ps_strip_html_headers(r);
  // See https://github.com/apache/incubator-pagespeed-ngx/issues/819
  ctx->location_field_set = r->headers_out.location != nullptr;

  // TODO(jefftk): is this thread safe?
  copy_response_headers_from_ngx(r, ctx->base_fetch->response_headers());

  ps_set_buffered(r, true);
  r->filter_need_in_memory = 1;
  return NGX_AGAIN;
}

ngx_int_t ps_html_rewrite_body_filter(ngx_http_request_t* r, ngx_chain_t* in) {
  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  if (ps_disabled(cfg_s)) {
    return ngx_http_next_body_filter(r, in);
  }

  if (r != r->main) {
    // Don't handle subrequests.
    return ngx_http_next_body_filter(r, in);
  }
  // Don't need to check for a cache flush; already did in
  // ps_html_rewrite_header_filter.

  ps_request_ctx_t* ctx = ps_get_request_context(r);

  if (ctx == nullptr || ctx->html_rewrite == false) {
    // ctx is null iff we've decided to pass through this request unchanged.
    return ngx_http_next_body_filter(r, in);
  }

  // We don't want to handle requests with errors, but we should be dealing with
  // that in the header filter and not initializing ctx.
  CHECK(r->err_status == 0);  // NOLINT

  // Convert any file-based buffers to memory buffers.  This is a defensive
  // fallback: with the module in its proper chain position most responses
  // arrive as memory, and any actual read is logged at info level in
  // ps_read_file_buffer so it cannot pass silently.
  for (ngx_chain_t* cl = in; cl != nullptr; cl = cl->next) {
    if (cl->buf->in_file) {
      ngx_int_t rc = ps_read_file_buffer(r, cl->buf);
      if (rc != NGX_OK) {
        return NGX_ERROR;
      }
    }
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "http pagespeed html rewrite body filter \"%V\"", &r->uri);

  if (in != nullptr) {
    // Send all input data to the proxy fetch.
    ps_send_to_pagespeed(r, ctx, cfg_s, in);
  }

  return ngx_http_next_body_filter(r, nullptr);
}

void ps_html_rewrite_filter_init() {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ps_html_rewrite_header_filter;

  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ps_html_rewrite_body_filter;
}

}  // namespace html_rewrite

using html_rewrite::ps_html_rewrite_filter_init;

namespace in_place {

ngx_http_output_header_filter_pt ngx_http_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_next_body_filter;

// Header filter to support IPRO.
//
// The control flow here is tricky, probably excessively so.
ngx_int_t ps_in_place_check_header_filter(ngx_http_request_t* r) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);

  if (ctx == nullptr) {
    return ngx_http_next_header_filter(r);
  }

  if (ctx->daemon_record_pending) {
    // The daemon substrate did no in-place lookup for this request, so this
    // pass is the only one this filter makes for it: build the recorder
    // here, at the same point in the filter chain where the classic path
    // hands its recorder the preliminary headers.
    ctx->daemon_record_pending = false;
    // NOT the HEAD guard, and not a 204/304 guard either: nginx's core
    // header filter sets header_only for a HEAD, a 204 and a 304 DOWNSTREAM
    // of this filter, so at this position the flag is effectively never
    // set -- the only main-request setter that can already have run is
    // nginx's post_action.  A HEAD never gets the mark (the method is
    // tested there), and status filtering is the recorder's: it refuses
    // anything but a 200.
    if (!r->header_only) {
      ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
      NgxServerContext* server_context = cfg_s->server_context;
      const SystemRewriteOptions* options =
          SystemRewriteOptions::DynamicCast(server_context->global_options());

      DaemonRecordRequest record_request;
      // The URL the rest of the module already agreed on and the classic
      // recorder already keys on: PageSpeed control parameters stripped,
      // X-Forwarded-Proto honoured.  The serve arm will key on the same
      // three fields.
      GoogleUrl full_url(ctx->url_string);
      full_url.PathAndLeaf().CopyToString(&record_request.url);
      full_url.Host().CopyToString(&record_request.hostname);
      full_url.Scheme().CopyToString(&record_request.scheme);

      RequestHeaders request_headers;
      copy_request_headers_from_ngx(r, &request_headers);
      RequestHeaders::Properties props = request_headers.GetProperties();
      // On nginx today this OR adds nothing: headers_in.user is only ever
      // set from a decodable Authorization: Basic header, which the header
      // test already covers.  It stays as the counterpart of Apache's
      // request_->user in case a module ever populates the field otherwise.
      // The real gap, here as on the classic path, is a request
      // authenticated WITHOUT an Authorization header -- auth_request, a
      // session or token scheme without cookies, client-certificate TLS:
      // such a response is recorded as unauthenticated.  Against that,
      // nginx still runs its access phase per request before any future
      // serve of the recorded entry.
      props.has_authorization =
          props.has_authorization || r->headers_in.user.len > 0;
      record_request.request_properties = props;

      // LookupJoined, not Lookup1: the capability headers are routinely
      // multi-token ("image/avif,image/webp,*/*", "gzip, deflate, br"), and
      // a comma-split value is several values, which Lookup1 reports as
      // absent -- the record key would carry a no-capability mask for every
      // such request, and the notification would ask the daemon for the
      // wrong variant family.
      record_request.accept =
          request_headers.LookupJoined(HttpAttributes::kAccept);
      record_request.user_agent =
          request_headers.LookupJoined(HttpAttributes::kUserAgent);
      record_request.save_data = request_headers.LookupJoined("Save-Data");
      record_request.accept_encoding =
          request_headers.LookupJoined(HttpAttributes::kAcceptEncoding);

      // The resolved configuration, stated as a value.  This port renders it
      // from the server-global options rather than from a per-request
      // resolution: a request carrying its own option overrides records
      // under the server's context, which asks the daemon for the server's
      // variants -- a missed optimization for that request, never a wrong
      // one.
      if (options != nullptr &&
          OptionContext::Compute(*options, &record_request.option_context,
                                 &record_request.option_signature) !=
              OptionContextStatus::kOk) {
        record_request.option_context.clear();
        record_request.option_signature.clear();
      }

      IproRecorder* recorder = nullptr;
      if (options != nullptr) {
        recorder = MakeDaemonIproRecorderIfOpen(
            server_context->daemon_adapter(), record_request,
            options->ComputeHttpOptions(), server_context->timer(),
            cfg_s->handler, server_context->rewrite_stats());
      }
      if (recorder != nullptr) {
        ctx->recorder = recorder;
        ctx->daemon_recorder = true;
        ctx->daemon_body_bytes = 0;
        r->filter_need_in_memory = 1;
        ResponseHeaders response_headers;
        copy_response_headers_from_ngx(r, &response_headers);
        recorder->ConsiderResponseHeaders(IproRecorder::kPreliminaryHeaders,
                                          &response_headers);
      } else if (server_context->daemon_health() == DaemonHealth::kReady &&
                 g_daemon_record_sequence != nullptr &&
                 ShouldPostDaemonOpenAttempt()) {
        // The verdict says the daemon is usable but this worker has no open
        // volume handle: record nothing for this request and ask for one
        // open, off the event thread, at most once per latch interval.
        PostDaemonOpenAttempt(g_daemon_record_sequence,
                              server_context->daemon_adapter());
      }
    }
    return ngx_http_next_header_filter(r);
  }

  if (ctx->recorder != nullptr) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ps in place check header filter recording: %V", &r->uri);

    CHECK(!ctx->in_place);

    // We didn't find this resource in cache originally, so we're recording it
    // as it passes us by.  At this point the headers from things that run
    // before us are set but not things that run after us, which means here is
    // where we need to check whether there's a "Content-Encoding: gzip".  If we
    // waited to do this in ps_in_place_body_filter we wouldn't be able to tell
    // the difference between response headers that have "C-E: gz" because we're
    // proxying for an upstream that gzipped the content and response headers
    // that have it because the gzip filter (which runs after us) is going to
    // produce gzipped output.
    //
    // The recorder will do this checking, so pass it the headers.
    ResponseHeaders response_headers;
    copy_response_headers_from_ngx(r, &response_headers);
    ctx->recorder->ConsiderResponseHeaders(IproRecorder::kPreliminaryHeaders,
                                           &response_headers);
    return ngx_http_next_header_filter(r);
  }

  if (!ctx->in_place) {
    return ngx_http_next_header_filter(r);
  }

  // If our request was finalized during the IPRO lookup, we decline.
  if (r->connection->error) {
    return ps_decline_request(r);
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "ps in place check header filter initial: %V", &r->uri);

  int status_code = r->headers_out.status;
  bool status_ok = (status_code != 0) && (status_code < 400);

  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  NgxServerContext* server_context = cfg_s->server_context;
  MessageHandler* message_handler = cfg_s->handler;
  GoogleString url = ps_determine_url(r);
  // The URL we use for cache key is a bit different since it may
  // have PageSpeed query params removed.
  GoogleString cache_url = ctx->url_string;

  // continue process
  if (status_ok) {
    ctx->in_place = false;

    server_context->rewrite_stats()->ipro_served()->Add(1);
    message_handler->Message(kInfo, "Serving rewritten resource in-place: %s",
                             url.c_str());

    return ngx_http_next_header_filter(r);
  }

  if (status_code == CacheUrlAsyncFetcher::kNotInCacheStatus &&
      !r->header_only) {
    server_context->rewrite_stats()->ipro_not_in_cache()->Add(1);
    server_context->message_handler()->Message(
        kInfo,
        "Could not rewrite resource in-place "
        "because URL is not in cache: %s",
        cache_url.c_str());
    // A HEAD is a miss like any other, but it gets no recorder: it has no
    // body to record, so a recorder could only end in an
    // incomplete-recording failure at teardown.  The guard is the METHOD,
    // not r->header_only: nginx sets header_only for a HEAD in its core
    // header filter, downstream of this one.  The header_only test above is
    // not dead: nginx's post_action sets the flag before its internal
    // redirect and keeps the method, and such a request has no body either.
    if (r->method & NGX_HTTP_HEAD) {
      return ps_decline_request(r);
    }
    const SystemRewriteOptions* options =
        SystemRewriteOptions::DynamicCast(ctx->driver->options());

    RequestContextPtr request_context(
        cfg_s->server_context->NewRequestContext(r));
    request_context->set_options(options->ComputeHttpOptions());
    RequestHeaders request_headers;
    copy_request_headers_from_ngx(r, &request_headers);
    // This URL was not found in cache (neither the input resource nor
    // a ResourceNotCacheable entry) so we need to get it into cache
    // (or at least a note that it cannot be cached stored there).
    // The in-place body filter feeds the recorder as the response streams
    // through.
    // The recorder is built through the record gate rather than here: that
    // function is the single place in the tree allowed to construct one, and
    // it returns nullptr for every substrate other than the classic one.
    // nginx passes the literal kClassic, so it always constructs.
    ctx->recorder = MakeIproRecorderIfClassic(
        IproDisposition::kClassic, request_context, cache_url,
        ctx->driver->CacheFragment(), request_headers.GetProperties(),
        options->ipro_max_response_bytes(),
        options->ipro_max_concurrent_recordings(), server_context->http_cache(),
        server_context->statistics(), message_handler);
    // With the literal kClassic the gate always constructs, so this branch
    // is always taken today; it exists because a disposition that returns no
    // recorder must not force the response body into memory.
    if (ctx->recorder != nullptr) {
      // set in memory flag for in_place_body_filter
      r->filter_need_in_memory = 1;
    }

    // We don't have the response headers at all yet because we haven't yet gone
    // to the backend.
  } else {
    server_context->rewrite_stats()->ipro_not_rewritable()->Add(1);
    message_handler->Message(kInfo, "Could not rewrite resource in-place: %s",
                             url.c_str());
  }

  return ps_decline_request(r);
}

// If we've decided that we should record this response for future optimization
// with IPRO, then log the bytes as they come through
ngx_int_t ps_in_place_body_filter(ngx_http_request_t* r, ngx_chain_t* in) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);

  if (ctx == nullptr || ctx->recorder == nullptr) {
    return ngx_http_next_body_filter(r, in);
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "ps in place body filter: %V", &r->uri);

  // Convert any file-based buffers to memory buffers for IPRO recording.
  for (ngx_chain_t* cl = in; cl != nullptr; cl = cl->next) {
    if (cl->buf->in_file) {
      ngx_int_t rc = ps_read_file_buffer(r, cl->buf);
      if (rc != NGX_OK) {
        return NGX_ERROR;
      }
    }
  }

  IproRecorder* recorder = ctx->recorder;
  for (ngx_chain_t* cl = in; cl; cl = cl->next) {
    if (ngx_buf_size(cl->buf)) {
      CHECK(ngx_buf_in_memory(cl->buf));
      StringPiece contents(reinterpret_cast<char*>(cl->buf->pos),
                           ngx_buf_size(cl->buf));
      recorder->Write(contents, recorder->handler());
      if (ctx->daemon_recorder) {
        ctx->daemon_body_bytes += contents.size();
      }
    }

    if (cl->buf->flush) {
      recorder->Flush(recorder->handler());
    }

    if (cl->buf->last_buf || recorder->failed()) {
      if (ctx->daemon_recorder) {
        // DoneAndSetHeaders opens the volume write and may notify the
        // daemon: blocking work, so it runs on the record arm's own pool,
        // not here.  The closure owns the recorder and the copied final
        // headers from here on; the pool's shutdown completes the recorder
        // even if the closure never runs.
        ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
        ResponseHeaders* headers = new ResponseHeaders();
        copy_response_headers_from_ngx(r, headers);
        // The drop counter lives on this server context's statistics object,
        // looked up per completion: there are several statistics objects
        // (the global one and each server's), so a process-wide cached
        // Variable* would count against whichever was registered first.
        // FindVariable, not GetVariable: a statistics object that ever
        // misses the registration degrades to an uncounted drop rather than
        // aborting the worker on GetVariable's CHECK.
        Variable* dropped = cfg_s->server_context->statistics()->FindVariable(
            kIproDaemonRecordDropped);
        // The queue reservation is clamped at the recorder's own content
        // cap: Write() stops buffering there and clears, so the bytes the
        // closure will actually hold never exceed it, and a reservation
        // larger than that only inflates the drop counter.
        const int64_t body_bytes =
            std::min(ctx->daemon_body_bytes,
                     static_cast<int64_t>(kDaemonOriginalContentCapBytes));
        if (g_daemon_record_sequence != nullptr &&
            TryQueueDaemonCompletion(body_bytes, dropped)) {
          g_daemon_record_sequence->Add(new DaemonRecordCompletion(
              recorder, headers, cl->buf->last_buf, body_bytes));
        } else {
          // No queue room (or no pool): the recording is finished inline as
          // incomplete, which stores nothing.  The response is unaffected.
          // The cap already counted the no-room case; a missing pool counts
          // here, so every refused completion shows up in the drop counter.
          if (g_daemon_record_sequence == nullptr && dropped != nullptr) {
            dropped->Add(1);
          }
          recorder->DoneAndSetHeaders(nullptr, false);
          delete headers;
        }
        ctx->recorder = nullptr;
        ctx->daemon_recorder = false;
      } else {
        ResponseHeaders response_headers;
        copy_response_headers_from_ngx(r, &response_headers);
        ctx->recorder->DoneAndSetHeaders(
            &response_headers,
            cl->buf->last_buf /* response is complete if last_buf is set */);
        ctx->recorder = nullptr;
      }
      break;
    }
  }

  return ngx_http_next_body_filter(r, in);
}

void ps_in_place_filter_init() {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ps_in_place_check_header_filter;

  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ps_in_place_body_filter;
}

}  // namespace in_place

using in_place::ps_in_place_filter_init;

ngx_int_t send_out_headers_and_body(ngx_http_request_t* r,
                                    const ResponseHeaders& response_headers,
                                    const GoogleString& output) {
  ngx_int_t rc =
      copy_response_headers_to_ngx(r, response_headers, kDontPreserveHeaders);

  if (rc != NGX_OK) {
    return NGX_ERROR;
  }

  rc = ngx_http_send_header(r);

  if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
    return rc;
  }

  // Send the body.
  ngx_chain_t* out;
  rc = string_piece_to_buffer_chain(r->pool, output, &out,
                                    true /* send_last_buf */, false);
  if (rc == NGX_ERROR) {
    return NGX_ERROR;
  }
  CHECK(rc == NGX_OK);

  return ngx_http_output_filter(r, out);
}

// Handle responses where we have the content we need in memory and can just
// send it right out.
ngx_int_t ps_simple_handler(ngx_http_request_t* r,
                            NgxServerContext* server_context,
                            RequestRouting::Response response_category) {
  NgxRewriteDriverFactory* factory =
      static_cast<NgxRewriteDriverFactory*>(server_context->factory());
  NgxMessageHandler* message_handler = factory->ngx_message_handler();
  StringPiece request_uri_path = str_to_string_piece(r->uri);

  GoogleString url_string = ps_determine_url(r);
  GoogleUrl url(url_string);
  QueryParams query_params;
  if (url.IsWebValid()) {
    query_params.ParseFromUrl(url);
  }

  GoogleString output;
  StringWriter writer(&output);
  HttpStatus::Code status = HttpStatus::kOK;
  ContentType content_type = kContentTypeHtml;
  StringPiece cache_control = HttpAttributes::kNoCache;
  // Backing store for cache_control when a case below points it at a
  // fetch's response headers, which do not outlive the switch block.
  GoogleString fetch_cache_control;
  const char* error_message = nullptr;

  switch (response_category) {
    case RequestRouting::kStaticContent: {
      StringPiece file_contents;
      // Answered only when the server's path and the module's own reading of
      // the request path name the same file under the static asset prefix;
      // anything else is left to the server.
      const StringPiece asset_name = NgxStaticAssetName(
          request_uri_path,
          url.IsWebValid() ? url.PathSansQuery() : StringPiece(),
          factory->static_asset_prefix());
      if (asset_name.empty() ||
          !server_context->static_asset_manager()->GetAsset(
              asset_name, &file_contents, &content_type, &cache_control)) {
        return NGX_DECLINED;
      }
      file_contents.CopyToString(&output);
      break;
    }
    case RequestRouting::kMessages: {
      // Defense-in-depth: warn once per process if a non-loopback client
      // reaches the message-history endpoint. The web-server-layer ACL is
      // expected to gate this; we only signal that it wasn't effective.
      ps_warn_admin_exposure(r, server_context, response_category);
      // Same JSON as the other ports: run the shared handler into a string
      // fetch and hand its body to nginx's writer.
      GoogleString json;
      RequestContextPtr request_context(server_context->NewRequestContext(r));
      request_context->set_options(
          server_context->config()->ComputeHttpOptions());
      StringAsyncFetch fetch(request_context, &json);
      fetch.request_headers()->set_method(RequestHeaders::MethodFromString(
          str_to_string_piece(r->method_name)));
      QueryParams message_query_params;
      message_query_params.ParseFromUntrustedString(
          str_to_string_piece(r->args));
      server_context->MessageHistoryHandler(*server_context->config(),
                                            AdminSite::kOther,
                                            message_query_params, &fetch);
      // ps_simple_handler's shared post-switch code pins content_type and
      // cache_control to defaults; take the JSON type and cache policy the
      // shared handler set instead.
      const ContentType* fetch_content_type =
          fetch.response_headers()->DetermineContentType();
      if (fetch_content_type != nullptr) {
        content_type = *fetch_content_type;
      }
      fetch_cache_control =
          fetch.response_headers()->LookupJoined(HttpAttributes::kCacheControl);
      if (!fetch_cache_control.empty()) {
        cache_control = fetch_cache_control;
      }
      writer.Write(json, message_handler);
      break;
    }
    default:
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "ps_simple_handler: unknown RequestRouting.");
      return NGX_ERROR;
  }

  if (error_message != nullptr) {
    status = HttpStatus::kNotFound;
    content_type = kContentTypeHtml;
    output = error_message;
  }

  ResponseHeaders response_headers;
  response_headers.SetStatusAndReason(status);
  response_headers.set_major_version(1);
  response_headers.set_minor_version(1);

  response_headers.Add(HttpAttributes::kContentType, content_type.mime_type());
  // http://msdn.microsoft.com/en-us/library/ie/gg622941(v=vs.85).aspx
  // Script and styleSheet elements will reject responses with
  // incorrect MIME types if the server sends the response header
  // "X-Content-Type-Options: nosniff". This is a security feature
  // that helps prevent attacks based on MIME-type confusion.
  response_headers.Add("X-Content-Type-Options", "nosniff");

  int64 now_ms = factory->timer()->NowMs();
  response_headers.SetDate(now_ms);
  response_headers.SetLastModified(now_ms);
  response_headers.Add(HttpAttributes::kCacheControl, cache_control);

  char* cache_control_s = string_piece_to_pool_string(r->pool, cache_control);
  if (cache_control_s != nullptr) {
    if (FindIgnoreCase(cache_control, "private") == StringPiece::npos) {
      response_headers.Add(HttpAttributes::kEtag, "W/\"0\"");
    }
  }

  return send_out_headers_and_body(r, response_headers, output);
}

void ps_beacon_handler_helper(ngx_http_request_t* r, StringPiece beacon_data) {
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "ps_beacon_handler_helper: beacon[%d] %*s", beacon_data.size(),
                beacon_data.size(), beacon_data.data());

  StringPiece user_agent;
  if (r->headers_in.user_agent != nullptr) {
    user_agent = str_to_string_piece(r->headers_in.user_agent->value);
  }

  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  CHECK(cfg_s != nullptr);

  RequestContextPtr request_context(
      cfg_s->server_context->NewRequestContext(r));
  // TODO(sligocki): Do we want custom options here? It probably doesn't matter
  // for beacons.
  request_context->set_options(
      cfg_s->server_context->global_options()->ComputeHttpOptions());

  cfg_s->server_context->HandleBeacon(beacon_data, user_agent, request_context);

  ps_set_cache_control(r, const_cast<char*>("max-age=0, no-cache"));

  // TODO(jefftk): figure out how to insert Content-Length:0 as a response
  // header so wget doesn't hang.
}

// Load the request body into out.  ngx_http_read_client_request_body must
// already have been called.  Return false on failure, true on success.
bool ps_request_body_to_string_piece(ngx_http_request_t* r, StringPiece* out) {
  if (r->request_body == nullptr || r->request_body->bufs == nullptr) {
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                  "ps_request_body_to_string_piece: "
                  "empty request body.");
    return false;
  }

  if (r->request_body->temp_file) {
    u_char buf[POST_BUF_READ_SIZE];
    int count = 0;
    ssize_t ret;
    GoogleString tmp;

    // Note that we depend on nginx to impose sensible limits on post data.
    while ((ret = ngx_read_file(&r->request_body->temp_file->file, buf,
                                POST_BUF_READ_SIZE, count)) > 0) {
      tmp.append(reinterpret_cast<char*>(buf), ret);
      count += ret;
    }

    if (ret < 0) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "ps_request_body_to_string_piece: "
                    "error reading post body.");
      return false;
    }

    // We have read the complete post body, copy it to a buffer
    // from the request's pool.
    u_char* data = reinterpret_cast<u_char*>(ngx_palloc(r->pool, count));
    memcpy(data, tmp.c_str(), count);
    *out = StringPiece(reinterpret_cast<char*>(data), count);

    return true;
  } else if (r->request_body->bufs->next == nullptr) {
    // There's just one buffer, so we can simply return a StringPiece pointing
    // to this buffer.
    ngx_buf_t* buffer = r->request_body->bufs->buf;
    CHECK(!buffer->in_file);
    int len = buffer->last - buffer->pos;
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "ngx_pagespeed beacon: single buffer of %d", len);
    *out = StringPiece(reinterpret_cast<char*>(buffer->pos), len);
    return true;
  } else {
    // There are multiple buffers, so we need to allocate memory for a string to
    // hold the whole result.  This should only happen when the POST is sent
    // with "Transfer-Encoding: Chunked".

    // First determine how much data there is.
    int len = 0;
    int buffers = 0;

    ngx_chain_t* chain_link;
    for (chain_link = r->request_body->bufs; chain_link != nullptr;
         chain_link = chain_link->next) {
      len += chain_link->buf->last - chain_link->buf->pos;
      buffers++;
    }

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "ngx_pagespeed beacon: %d buffers totalling %d", len);

    // Allocate a string to store the combined result.
    u_char* s = static_cast<u_char*>(ngx_palloc(r->pool, len));
    if (s == nullptr) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "ps_request_body_to_string_piece: "
                    "failed to allocate memory");
      return false;
    }

    // Copy the data into the combined string.
    u_char* current_position = s;
    int i;
    for (chain_link = r->request_body->bufs, i = 0; chain_link != nullptr;
         chain_link = chain_link->next, i++) {
      ngx_buf_t* buffer = chain_link->buf;
      CHECK(!buffer->in_file);
      current_position =
          ngx_copy(current_position, buffer->pos, buffer->last - buffer->pos);
    }
    CHECK_EQ(current_position, s + len);
    *out = StringPiece(reinterpret_cast<char*>(s), len);
    return true;
  }
}

// Parses out query params from the request.
void ps_query_params_handler(ngx_http_request_t* r, StringPiece* data) {
  StringPiece unparsed_uri = str_to_string_piece(r->unparsed_uri);
  stringpiece_ssize_type question_mark_index = unparsed_uri.find("?");
  if (question_mark_index == StringPiece::npos) {
    *data = "";
  } else {
    *data =
        unparsed_uri.substr(question_mark_index + 1,
                            unparsed_uri.size() - (question_mark_index + 1));
  }
}

// Called after nginx reads the POST body for an admin request.
// Delegates to ps_resource_handler which will find the body in
// r->request_body and pass it to SystemServerContext::AdminPage.
void ps_admin_body_handler(ngx_http_request_t* r) {
  RequestRouting::Response response_category = ps_route_request(r);
  ngx_int_t rc = ps_resource_handler(r, false, response_category);
  if (rc != NGX_DONE) {
    ngx_http_finalize_request(r, rc);
  }
}

// Called after nginx reads the request body from the client.  For another
// example processing request buffers, see ngx_http_form_input_module.c
void ps_beacon_body_handler(ngx_http_request_t* r) {
  // Even if the beacon is a POST, the originating url should be in the query
  // params, not the POST body.
  StringPiece query_param_beacon_data;
  ps_query_params_handler(r, &query_param_beacon_data);

  StringPiece request_body;
  bool ok = ps_request_body_to_string_piece(r, &request_body);
  GoogleString beacon_data = StrCat(query_param_beacon_data, "&", request_body);
  if (ok) {
    ps_beacon_handler_helper(r, beacon_data.c_str());
    ngx_http_finalize_request(r, NGX_HTTP_NO_CONTENT);
  } else {
    ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
  }
}

// We need to get the beacon data to ps_beacon_body_handler so it can pass it
// along to SystemServerContext::HandleBeacon, but it might have been POSTed.
// If it's posted we need to make an async call to get the data, otherwise just
// read it out of the query params.
ngx_int_t ps_beacon_handler(ngx_http_request_t* r) {
  if (r->method == NGX_HTTP_POST) {
    // Use post body. Handler functions are called before the request body has
    // been read from the client, so we need to ask nginx to read it from the
    // client and then call us back.  Control flow continues in
    // ps_beacon_body_handler unless there's an error reading the request body.
    //
    // See: http://forum.nginx.org/read.php?2,31312,31312
    ngx_int_t rc = ngx_http_read_client_request_body(r, ps_beacon_body_handler);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
      return rc;
    }
    return NGX_DONE;
  } else {
    // Use query params.
    StringPiece query_param_beacon_data;
    ps_query_params_handler(r, &query_param_beacon_data);
    ps_beacon_handler_helper(r, query_param_beacon_data);
    return NGX_HTTP_NO_CONTENT;
  }
}

// Some things pagespeed filters on the way past (html) and other things it
// actually handles, like requests for resources
// (example.css.pagespeed.ce.LyfcM6Wulf.css) and static content
// (/ngx_pagespeed_static/js_defer.q1EBmcgYOC.js).  This is called once we know
// we need to handle a resource, and it figures out which particular handler can
// supply it to the user.
ngx_int_t ps_content_handler(ngx_http_request_t* r) {
  ps_srv_conf_t* cfg_s = ps_get_srv_config(r);
  if (ps_disabled(cfg_s)) {
    // Pagespeed is not on for this server block.
    return NGX_DECLINED;
  }

  // Poll for cache flush on every request (polls are rate-limited).
  cfg_s->server_context->FlushCacheIfNecessary();

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "http pagespeed handler \"%V\"", &r->uri);

  RequestRouting::Response response_category = ps_route_request(r);
  switch (response_category) {
    case RequestRouting::kError:
      return NGX_ERROR;
    case RequestRouting::kPagespeedDisabled:
    case RequestRouting::kInvalidUrl:
    case RequestRouting::kPagespeedSubrequest:
    case RequestRouting::kErrorResponse:
      return NGX_DECLINED;
    case RequestRouting::kBeacon:
      return ps_beacon_handler(r);
    case RequestRouting::kWebBotAuthCounter: {
      // Opt-in counter (experimental). Build the coarse/exact doc
      // (or hide it) in the webbotauth handler, then emit it. Hiding (mode
      // private + no valid token) declines -> normal 404. HEAD is honored by
      // send_out_headers_and_body (r->header_only sends headers, no body).
      ResponseHeaders response_headers;
      GoogleString body;
      if (!ps_webbotauth_counter_build_response(r, &response_headers, &body)) {
        return NGX_DECLINED;
      }
      return send_out_headers_and_body(r, response_headers, body);
    }
    case RequestRouting::kStaticContent:
    case RequestRouting::kMessages:
      return ps_simple_handler(r, cfg_s->server_context, response_category);
    case RequestRouting::kStatistics:
    case RequestRouting::kGlobalStatistics:
    case RequestRouting::kConsole:
    case RequestRouting::kAdmin:
    case RequestRouting::kGlobalAdmin:
      if (r->method == NGX_HTTP_POST) {
        // POST requests need the body read before we can handle them.
        // Control flow continues in ps_admin_body_handler.
        ngx_int_t rc =
            ngx_http_read_client_request_body(r, ps_admin_body_handler);
        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
          return rc;
        }
        return NGX_DONE;
      }
      return ps_resource_handler(r, false /* html rewrite */,
                                 response_category);
    case RequestRouting::kCachePurge:
    case RequestRouting::kResource:
      return ps_resource_handler(r, false /* html rewrite */,
                                 response_category);
  }

  CHECK(0);
  return NGX_ERROR;
}

ngx_int_t ps_phase_handler(ngx_http_request_t* r,
                           ngx_http_phase_handler_t* ph) {
  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed phase: %ui", r->phase_handler);

  r->write_event_handler = ngx_http_request_empty_handler;

  ngx_int_t rc = ps_content_handler(r);
  // Warning: this requires ps_content_handler to always return NGX_DECLINED
  // directly if it's not going to handle the request. It is not ok for
  // ps_content_handler to asynchronously determine whether to handle the
  // request, returning NGX_DONE here.
  if (rc == NGX_DECLINED) {
    r->write_event_handler = ngx_http_core_run_phases;
    r->phase_handler++;
    return NGX_AGAIN;
  }

  ngx_http_finalize_request(r, rc);
  return NGX_OK;
}

namespace fix_headers {
ngx_http_output_header_filter_pt ngx_http_next_header_filter;

// Clear a few headers from html responses.

ngx_int_t ps_html_rewrite_fix_headers_filter(ngx_http_request_t* r) {
  ps_request_ctx_t* ctx = ps_get_request_context(r);
  if (r != r->main || ctx == nullptr || !ctx->html_rewrite ||
      ctx->preserve_caching_headers == kPreserveAllCachingHeaders) {
    return ngx_http_next_header_filter(r);
  }
  if (ctx->preserve_caching_headers == kDontPreserveHeaders) {
    // Don't cache html.  See mod_instaweb:instaweb_fix_headers_filter.
    NgxCachingHeaders caching_headers(r);
    ps_set_cache_control(
        r, string_piece_to_pool_string(
               r->pool, caching_headers.GenerateDisabledCacheControl()));
  }

  // Pagespeed html doesn't need etags: it should never be cached.
  ngx_http_clear_etag(r);

  // An html page may change without the underlying file changing, because of
  // how resources are included.  Pagespeed adds cache control headers for
  // resources instead of using the last modified header.
  ngx_http_clear_last_modified(r);

  // Clear expires
  if (r->headers_out.expires) {
    r->headers_out.expires->hash = 0;
    r->headers_out.expires = nullptr;
  }

  return ngx_http_next_header_filter(r);
}

void ps_html_rewrite_fix_headers_filter_init() {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ps_html_rewrite_fix_headers_filter;
}

}  // namespace fix_headers

using fix_headers::ps_html_rewrite_fix_headers_filter_init;

// ---------------------------------------------------------------------------
// Runtime nginx-ABI guard.
//
// nginx's dynamic-module load check (NGX_MODULE_SIGNATURE + module->version)
// makes the module and the running nginx agree on TYPE SIZES and build options,
// but NOT on the field layout of core request structs. A distro can insert a
// field into e.g. ngx_http_headers_in_t in a security update WITHOUT changing
// the upstream version or the compat signature -- Ubuntu's CVE-2026-49975 did
// exactly this (+ngx_uint_t count), shifting every ngx_http_request_t field
// after headers_in -- including r->phase_handler -- by 8 bytes. A module built
// against the pre-patch layout then reads r->phase_handler at the wrong offset,
// mis-installs its content checker, and the worker SPINS forever (silent hang).
// `nginx -t` still passes, so nothing catches it at load time.
//
// This module is built against the distro's own patched nginx source, so
// offsets match at ship time; this guard is the backstop for AFTER ship,
// when a customer upgrades nginx past the revision we built against.
//
// We cannot reject at load time: nginx exposes neither its struct layout nor
// its distro revision at runtime, and we cannot change the running nginx's load
// check. We CAN detect it at the first request, cheaply and safely. When nginx
// invokes ps_preaccess_handler via ngx_http_core_generic_phase, r->phase_handler
// MUST index this very handler's slot (ph[r->phase_handler].handler ==
// ps_preaccess_handler). cmcf->phase_engine lives in the core MAIN conf and
// r->ctx sits before headers_in, so both read correctly even under the shift --
// only r->phase_handler (a request field after headers_in) is wrong. If the
// index no longer points back at us, the layout has drifted: we degrade the
// whole module to pass-through (return NGX_DECLINED, letting nginx's generic
// phase advance its OWN phase_handler) and log once. No hang; one clear,
// actionable log line instead of a wedged worker.
ngx_int_t ps_preaccess_handler(
    ngx_http_request_t* r);  // fwd decl for the guard

bool g_ps_abi_mismatch = false;

// Bounded: walk to the NULL-checker terminator so a wild (shifted) index can
// never read out of bounds. Real phase engines have ~a dozen entries.
bool ps_phase_index_is_ours(ngx_http_phase_handler_t* ph, ngx_uint_t i) {
  if (ph == nullptr) {
    return false;
  }
  ngx_uint_t n = 0;
  while (ph[n].checker != nullptr) {
    if (++n > 4096) {  // sanity cap; never reached by a real phase engine
      return false;
    }
  }
  return i < n && ph[i].handler == ps_preaccess_handler;
}

void ps_report_abi_mismatch(ngx_http_request_t* r) {
  if (g_ps_abi_mismatch) {
    return;  // already reported + degraded this worker
  }
  g_ps_abi_mismatch = true;
  ngx_log_error(
      NGX_LOG_ALERT, r->connection->log, 0,
      "ngx_pagespeed: nginx ABI mismatch detected -- r->phase_handler "
      "does not index this module's handler, so the running nginx's "
      "core request-struct layout differs from this module's build "
      "target (nginx %s; the running nginx's exact build is not "
      "introspectable at runtime). This typically means a distro "
      "security update changed nginx's struct layout without a version "
      "bump (see CVE-2026-49975). PageSpeed optimization is "
      "now DISABLED (pass-through) on this worker to prevent a worker "
      "hang. Reinstall nginx-module-pagespeed built for your current "
      "nginx, or contact We-Amp.",
      NGINX_VERSION);
}

// preaccess_handler should be at generic phase before try_files
ngx_int_t ps_preaccess_handler(ngx_http_request_t* r) {
  ngx_http_core_main_conf_t* cmcf;
  ngx_http_phase_handler_t* ph;
  ngx_uint_t i;

  cmcf = static_cast<ngx_http_core_main_conf_t*>(
      ngx_http_get_module_main_conf(r, ngx_http_core_module));

  ph = cmcf->phase_engine.handlers;

  i = r->phase_handler;

  // Runtime nginx-ABI guard -- BEFORE touching the phase engine.
  // If the running nginx's request-struct layout has drifted from our build
  // target, i (= r->phase_handler, read at this module's offset) no longer
  // points at our own slot; proceeding would corrupt the phase engine and hang
  // the worker. Degrade to pass-through instead. nginx's generic-phase checker
  // (which called us) then advances its OWN r->phase_handler correctly.
  //
  // Note: on a healthy worker this runs only on the FIRST request -- once we
  // install ps_phase_handler below (ph[i].checker), nginx invokes that checker
  // directly on later requests and this handler is not re-entered. The layout
  // is fixed for the worker's lifetime, and g_ps_abi_mismatch latches the
  // degraded state for every subsequent request, so checking once is sufficient.
  if (g_ps_abi_mismatch || !ps_phase_index_is_ours(ph, i)) {
    ps_report_abi_mismatch(r);
    return NGX_DECLINED;
  }

// move handlers before try_files && content phase
// As of nginx 1.13.4 we will be right before the try_files module
#if (nginx_version < NGINX_1_13_4)
  while (ph[i + 1].checker != ngx_http_core_try_files_phase &&
         ph[i + 1].checker != ngx_http_core_content_phase) {
    ph[i] = ph[i + 1];
    ph[i].next--;
    i++;
  }
#endif

  // insert ps phase handler
  ph[i].checker = ps_phase_handler;
  ph[i].handler = nullptr;
  ph[i].next = i + 1;

  // next preaccess handler
  r->phase_handler--;
  return NGX_DECLINED;
}

ngx_int_t ps_etag_filter_init(ngx_conf_t* cf) {
  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_conf_get_module_main_conf(cf, ngx_pagespeed));
  if (cfg_m != nullptr && cfg_m->driver_factory != nullptr) {
    ngx_http_ef_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ps_etag_header_filter;
  }
  return NGX_OK;
}

// Called before configuration.
ngx_int_t ps_pre_init(ngx_conf_t* cf) {
  // Setup an intervention setter for gzip configuration and check
  // gzip configuration command signatures.
  g_gzip_setter.Init(cf);

  // Register $x_verified_bot unconditionally so it always resolves (e.g. in
  // log_format / add_header) even before any pagespeed directive is parsed.
  // The get-handler is a no-op (not_found) unless WebBotAuth is enabled.
  ngx_str_t xvb_name = ngx_string("x_verified_bot");
  ngx_http_variable_t* xvb_var =
      ngx_http_add_variable(cf, &xvb_name, NGX_HTTP_VAR_CHANGEABLE);
  if (xvb_var == nullptr) {
    return NGX_ERROR;
  }
  xvb_var->get_handler = ps_x_verified_bot_variable;

  return NGX_OK;
}

ngx_int_t ps_init(ngx_conf_t* cf) {
  // Only put register pagespeed code to run if there was a "pagespeed"
  // configuration option set in the config file.  With "pagespeed off" we
  // consider every request and choose not to do anything, while with no
  // "pagespeed" directives we won't have any effect after nginx is done loading
  // its configuration.

  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_conf_get_module_main_conf(cf, ngx_pagespeed));

  // The driver factory is on the main config and is non-NULL iff there is a
  // pagespeed configuration option in the main config or a server block.  Note
  // that if any server block has pagespeed 'on' then our header filter, body
  // filter, and content handler will run in every server block.  This is ok,
  // because they will notice that the server context is NULL and do nothing.
  if (cfg_m->driver_factory != nullptr) {
    // The filter init order is important.
    ps_in_place_filter_init();

    ps_html_rewrite_fix_headers_filter_init();
    ps_base_fetch::ps_base_fetch_filter_init();
    ps_html_rewrite_filter_init();

    ngx_http_core_main_conf_t* cmcf = static_cast<ngx_http_core_main_conf_t*>(
        ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module));

    int phase = NGX_HTTP_PREACCESS_PHASE;

    // As of nginx 1.13.4, try_files has changed.
    // https://github.com/nginx/nginx/commit/129b06dc5dfab7b4513a4f274b3778cd9b8a6a22
#if (nginx_version >= NGINX_1_13_4)
    phase = NGX_HTTP_PRECONTENT_PHASE;
#endif

    ngx_http_handler_pt* h = static_cast<ngx_http_handler_pt*>(
        ngx_array_push(&cmcf->phases[phase].handlers));

    if (h == nullptr) {
      return NGX_ERROR;
    }
    *h = ps_preaccess_handler;

    // Observe-only Web-Bot-Auth classifier, default-off. Runs in
    // the same phase; early-returns NGX_DECLINED unless WebBotAuth is enabled.
    ngx_http_handler_pt* wba_h = static_cast<ngx_http_handler_pt*>(
        ngx_array_push(&cmcf->phases[phase].handlers));
    if (wba_h == nullptr) {
      return NGX_ERROR;
    }
    *wba_h = ps_webbotauth_preaccess_handler;

    // RSL-CAP enforcement, default-off. Registered last, and nginx installs
    // same-phase handlers in REVERSE registration order, so this runs FIRST
    // in the phase (before the observe-only classifier above);
    // early-returns NGX_DECLINED unless RslCapEnforcement is enabled,
    // otherwise maps the validator verdict to an inline 401/402.
    ngx_http_handler_pt* rce_h = static_cast<ngx_http_handler_pt*>(
        ngx_array_push(&cmcf->phases[phase].handlers));
    if (rce_h == nullptr) {
      return NGX_ERROR;
    }
    *rce_h = ps_rsl_cap_preaccess_handler;

    // Index $x_verified_bot so the preaccess handler can store the verdict once
    // (computed-once); the variable get-handler then reads the stored value.
    ngx_str_t xvb_index_name = ngx_string("x_verified_bot");
    ps_webbotauth_set_var_index(
        ngx_http_get_variable_index(cf, &xvb_index_name));
  }

  return NGX_OK;
}

ngx_http_module_t ps_etag_filter_module = {
    nullptr,              // preconfiguration
    ps_etag_filter_init,  // postconfiguration
    nullptr,
    nullptr,  // initialize main configuration
    nullptr,
    nullptr,
    nullptr,
    nullptr};

ngx_http_module_t ps_module = {ps_pre_init,  // preconfiguration
                               ps_init,      // postconfiguration

                               ps_create_main_conf,
                               nullptr,  // initialize main configuration

                               ps_create_srv_conf,
                               ps_merge_srv_conf,

                               ps_create_loc_conf,
                               ps_merge_loc_conf};

// called after configuration is complete, but before nginx starts forking
ngx_int_t ps_init_module(ngx_cycle_t* cycle) {
  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_cycle_get_module_main_conf(cycle, ngx_pagespeed));

  // See https://github.com/apache/incubator-pagespeed-ngx/issues/1220
  if (cfg_m == nullptr) {
    return NGX_OK;
  }

  ngx_http_core_main_conf_t* cmcf = static_cast<ngx_http_core_main_conf_t*>(
      ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module));
  ngx_http_core_srv_conf_t** cscfp =
      static_cast<ngx_http_core_srv_conf_t**>(cmcf->servers.elts);
  ngx_uint_t s;

  std::vector<SystemServerContext*> server_contexts;
  // Iterate over all configured server{} blocks to collect the server contexts.
  for (s = 0; s < cmcf->servers.nelts; s++) {
    ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
        cscfp[s]->ctx->srv_conf[ngx_pagespeed.ctx_index]);
    if (cfg_s->server_context != nullptr) {
      server_contexts.push_back(cfg_s->server_context);
    }
  }

  GoogleString error_message;
  int error_index = -1;
  Statistics* global_statistics = nullptr;
  cfg_m->driver_factory->PostConfig(server_contexts, &error_message,
                                    &error_index, &global_statistics);
  if (error_index != -1) {
    server_contexts[error_index]->message_handler()->Message(
        kError, "ngx_pagespeed is enabled. %s", error_message.c_str());
    return NGX_ERROR;
  }

  // Resolve every server's relationship with the optimizer daemon before any
  // worker is forked, so the serving path only ever reads a verdict.
  //
  // A refusal here fails the whole start, which is the point: the single
  // condition that refuses is a cache-volume sizing divergence between this
  // module and the daemon, and that divergence is SILENT at run time -- the
  // two sides would open different volume files, share nothing, and look
  // healthy while optimizing nothing.  Every other daemon problem degrades to
  // in-place optimization off with one loud line, and the server starts.
  //
  // On a reload the probe runs while the previous generation still holds the
  // volume: a peer that decides the volume needs a reset refuses the probe
  // open against a live holder, which this module reads as kUnavailable for
  // the whole new cycle even though the daemon is healthy.  The same overlap
  // exists during a binary upgrade, but there the exec'd binary's first cycle
  // IS the initial cycle, so the refusal below still fires and aborts the
  // upgrade -- which leaves the running master untouched.
  //
  // A config test must not touch the daemon at all: the probe open can
  // create a volume file, and Apache's post_config does not run under -t
  // either.
  if (!ngx_test_config) {
    // A refusal is only a refusal on the initial cycle.  A reload must never
    // exit out of a live master and orphan its workers, so on a later cycle
    // the verdict stays not-ready -- the module records and serves nothing
    // through a volume it did not attach to -- but the configuration keeps
    // running.  The adapter announces each distinct condition once per
    // process, so a refusal repeated across reloads would be silent; the
    // module therefore says so itself, every time.
    const bool initial_cycle =
        (cycle->old_cycle == nullptr || cycle->old_cycle->conf_ctx == nullptr);
    for (SystemServerContext* system_server_context : server_contexts) {
      NgxServerContext* server_context =
          dynamic_cast<NgxServerContext*>(system_server_context);
      CHECK(server_context != nullptr);
      if (!server_context->RunDaemonStartupCheck()) {
        if (initial_cycle) {
          return NGX_ERROR;
        }
        cfg_m->handler->Message(
            kError,
            "the optimizer daemon startup check refused this configuration; "
            "this reload keeps running with in-place optimization off");
      }
    }
  }

  if (!server_contexts.empty()) {
    // TODO(oschaaf): this ignores sigpipe messages from memcached.
    // however, it would be better to not have those signals generated
    // in the first place, as suppressing them this way may interfere
    // with other modules that actually are interested in these signals
    ps_ignore_sigpipe();

    // If no shared-mem statistics are enabled, then init using the default
    // NullStatistics.
    if (global_statistics == nullptr) {
      NgxRewriteDriverFactory::InitStats(cfg_m->driver_factory->statistics());
    }

    ngx_http_core_loc_conf_t* clcf = static_cast<ngx_http_core_loc_conf_t*>(
        ngx_http_conf_get_module_loc_conf((*cscfp), ngx_http_core_module));

    cfg_m->driver_factory->set_resolver(clcf->resolver);
    cfg_m->driver_factory->set_resolver_timeout(clcf->resolver_timeout);

    if (!cfg_m->driver_factory->CheckResolver()) {
      cfg_m->handler->Message(
          kError, "UseNativeFetcher is on, please configure a resolver.");
      return NGX_ERROR;
    }

    cfg_m->driver_factory->LoggingInit(cycle->log, true);
    cfg_m->driver_factory->RootInit();

    // Opt-in counter (experimental): map (or create on first run)
    // the shared counter file HERE in the master, before workers fork, so the
    // MAP_SHARED region is inherited by every worker and they all increment the
    // same physical counters. Only when some server has Web Bot Auth enabled or
    // a non-off counter mode (no file is created when the feature is unused).
    // The path is PAGESPEED_WEB_BOT_AUTH_COUNTER_FILE if set (read from the
    // master's launch environment), else derived from the first configured
    // FileCachePath (a guaranteed nginx-writable directory).
    {
      bool want_counter = false;
      GoogleString cache_dir;
      for (s = 0; s < cmcf->servers.nelts; s++) {
        ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
            cscfp[s]->ctx->srv_conf[ngx_pagespeed.ctx_index]);
        if (cfg_s->server_context == nullptr) {
          continue;
        }
        NgxRewriteOptions* opt = cfg_s->server_context->config();
        if (opt == nullptr) {
          continue;
        }
        const GoogleString& mode = opt->web_bot_auth_public_counter();
        if (opt->web_bot_auth() || mode == "public" || mode == "private") {
          want_counter = true;
        }
        if (cache_dir.empty() && !opt->file_cache_path().empty()) {
          cache_dir = opt->file_cache_path();
        }
      }
      if (want_counter) {
        GoogleString counter_path;
        const char* env_file = getenv("PAGESPEED_WEB_BOT_AUTH_COUNTER_FILE");
        if (env_file != nullptr && env_file[0] != '\0') {
          counter_path = env_file;
        } else if (!cache_dir.empty()) {
          counter_path = webbotauth::WebBotAuthCounterPath(
              std::string(cache_dir.data(), cache_dir.size()));
        }
        if (!counter_path.empty()) {
          ps_webbotauth_counter_map(counter_path);
        } else {
          // The feature is enabled but no path resolved (no FileCachePath on any
          // server AND PAGESPEED_WEB_BOT_AUTH_COUNTER_FILE unset): the endpoint
          // will serve an all-zero document. Tell the operator.
          ngx_log_error(
              NGX_LOG_WARN, cycle->log, 0,
              "pagespeed: web-bot-auth opt-in counter is enabled but "
              "no counter file path could be resolved (set "
              "FileCachePath or "
              "PAGESPEED_WEB_BOT_AUTH_COUNTER_FILE); the endpoint will "
              "serve an all-zero document");
        }
      }
    }
  } else {
    delete cfg_m->driver_factory;
    cfg_m->driver_factory = nullptr;
    active_driver_factory = nullptr;
  }
  return NGX_OK;
}

// This worker's background key-directory warmers (one per configured
// remote directory). Each forked nginx worker has its own copy (fork semantics);
// stopped+joined in ps_exit_child_process BEFORE the factory tears down the
// fetcher/cache they borrow. Populated in ps_init_child_process.
std::vector<std::unique_ptr<webbotauth::KeyDirectoryWarmer> >
    g_key_directory_warmers;

void ps_exit_child_process(ngx_cycle_t* cycle) {
  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_cycle_get_module_main_conf(cycle, ngx_pagespeed));
  // Stop+join the warmer threads BEFORE the factory tears down the
  // fetcher/cache they borrow. Each Stop() signals quit and joins.
  for (size_t i = 0; i < g_key_directory_warmers.size(); ++i) {
    g_key_directory_warmers[i]->Stop();
  }
  g_key_directory_warmers.clear();
  NgxBaseFetch::Terminate();
  if (cfg_m != nullptr && cfg_m->driver_factory != nullptr) {
    cfg_m->driver_factory->ShutDown();
  }
  // Shut down and delete the record arm's own pool HERE, between the
  // factory's shutdown and the volume close below: it is a port-owned pool,
  // so the factory never touches it, and the close loop's contract -- every
  // thread that can still touch an adapter, or hold a handle it handed out,
  // is finished or joined first -- covers it exactly.  The join is
  // UNBOUNDED in the same way the close is: it waits out the one active
  // item, while everything still queued is cancelled and sends nothing.
  // That active item is bounded on every shape it can take: a record
  // completion's volume commit is bounded by the storage layer at tens of
  // seconds against a stopped peer, and a posted notification is the client
  // library's notify call, which does not wait for the daemon: against a
  // stopped daemon it returned in microseconds, and once the daemon's accept
  // queue was full it was refused at once (both measured with the serve
  // benchmark under tools/parity).  So the join waits out nothing that can
  // stall.  The sequence is deleted with the pool; no FreeSequence anywhere.
  if (g_daemon_record_pool != nullptr) {
    g_daemon_record_pool->ShutDown();
    delete g_daemon_record_pool;
    g_daemon_record_pool = nullptr;
    g_daemon_record_sequence = nullptr;
  }
  // Close every server context's daemon volume handle HERE rather than
  // leaving it to the adapter's destructor.  The destructor only runs when
  // the cycle pool is destroyed and its cleanup handler deletes the factory,
  // an ordering this module does not control; here the close sits at a point
  // the module does control, after the driver factory has shut down.  It is
  // not bounded by nginx either way: the close joins the storage layer's
  // background threads with no deadline, and the event loop (and with it
  // worker_shutdown_timeout) is already over when exit hooks run.
  //
  // What the factory shutdown guarantees is that the PSOL worker pools are
  // joined.  It does NOT cover drivers that outlive its bounded wait, nor any
  // thread this port owns itself: anything that can still touch an adapter,
  // or hold a handle it handed out, has to be finished or joined BEFORE this
  // loop.  The close invalidates a raw handle that holders copied.
  //
  // No process-kind gate: closing is a no-op where nothing was opened.  A
  // null driver factory means there are no server contexts, which the
  // per-server null check below covers, and a non-null main conf for this
  // module implies an http{} block, so the core main conf exists.
  if (cfg_m != nullptr) {
    ngx_http_core_main_conf_t* cmcf = static_cast<ngx_http_core_main_conf_t*>(
        ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module));
    ngx_http_core_srv_conf_t** cscfp =
        static_cast<ngx_http_core_srv_conf_t**>(cmcf->servers.elts);
    for (ngx_uint_t s = 0; s < cmcf->servers.nelts; s++) {
      ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
          cscfp[s]->ctx->srv_conf[ngx_pagespeed.ctx_index]);
      if (cfg_s->server_context != nullptr &&
          cfg_s->server_context->daemon_adapter() != nullptr) {
        cfg_s->server_context->daemon_adapter()->CloseRecordCache();
      }
    }
  }
  // Route any further LOG() to stderr and drop the NgxGLogSink (the nginx
  // counterpart of Apache's pagespeed_child_exit shutdown ordering).
  // Placement differs from Apache deliberately: cycle->log stays valid for the
  // whole hook, so teardown diagnostics above still reach the error log. The
  // hazard window this closes is AFTER the hook — a worker thread the factory
  // shutdown did not join that LOG()s after the cycle pool is destroyed. (The
  // spdlog path is already safe via the immortal held logger; the
  // flag protects the registered-sink path into ngx_log.)
  pagespeed_logging::ShutDownLogging();
  log_message_handler::ShutDown();
}

// Parse the operator refresh-seconds string. Empty/invalid -> 3600; clamped to
// a sane [60s, 86400s] window.
int64_t ParseWarmRefreshSec(const GoogleString& s) {
  int64_t v = 0;
  if (s.empty() || !StringToInt64(s, &v) || v <= 0) {
    v = 3600;
  }
  if (v < 60) v = 60;
  if (v > 86400) v = 86400;
  return v;
}

// Parse a comma-separated SSRF allowlist of https origins.
std::set<GoogleString> ParseWarmAllowlist(StringPiece spec) {
  std::set<GoogleString> out;
  StringPieceVector parts;
  SplitStringPieceToVector(spec, ",", &parts, true /* omit_empty */);
  for (size_t i = 0; i < parts.size(); ++i) {
    StringPiece p = parts[i];
    TrimWhitespace(&p);
    if (!p.empty()) {
      out.insert(GoogleString(p.data(), p.size()));
    }
  }
  return out;
}

// If a feature's remote key directory is fully configured, create + start a
// warmer for (realm,host,url). No-op when not configured (default-off). Dedups
// by (realm, cache-storage, host, url) within this worker via *seen. Requires
// the curl fetcher (the native nginx fetcher cannot complete a blocking fetch
// off the event loop) and a blocking metadata cache (the cache-only request
// reader can only read a blocking cache).
void MaybeStartKeyDirectoryWarmer(
    NgxRewriteDriverFactory* factory, NgxServerContext* server_context,
    const GoogleString& realm, const GoogleString& host,
    const GoogleString& url, const GoogleString& allowlist_spec,
    const GoogleString& refresh_spec, std::set<GoogleString>* seen) {
  if (host.empty() || url.empty() || allowlist_spec.empty()) {
    return;  // default-off: warm-fetch not configured
  }
  MessageHandler* handler = factory->message_handler();
  if (factory->use_native_fetcher()) {
    if (handler != nullptr) {
      handler->Message(kWarning,
                       "webbotauth A2: key-directory warm-fetch requires the "
                       "curl fetcher (UseNativeFetcher off); skipping %s",
                       url.c_str());
    }
    return;
  }
  CacheInterface* cache = server_context->metadata_cache();
  UrlAsyncFetcher* fetcher = server_context->DefaultSystemFetcher();
  if (cache == nullptr || fetcher == nullptr) {
    return;
  }
  // The cache-only request reader only consults a BLOCKING cache; on a
  // non-blocking metadata cache (e.g. an external memcached/redis backend) the
  // warmed keys would never be read, so skip rather than fetch uselessly.
  if (!cache->IsBlocking()) {
    if (handler != nullptr) {
      handler->Message(
          kWarning,
          "webbotauth A2: key-directory warm-fetch needs a blocking "
          "metadata cache; skipping %s",
          url.c_str());
    }
    return;
  }
  // Dedup per (realm, cache storage, host, url): vhosts that share a
  // FileCachePath share the metadata cache so one warmer suffices, while vhosts
  // with distinct cache paths each get their own (otherwise a second vhost's
  // cache would never be warmed).
  GoogleString cache_id;
  NgxRewriteOptions* opt = server_context->config();
  if (opt != nullptr) {
    cache_id = opt->file_cache_path();
  }
  const GoogleString dedup_key =
      StrCat(realm, "\n", cache_id, "\n", host, "\n", url);
  if (!seen->insert(dedup_key).second) {
    return;  // already warming this directory into this cache in this worker
  }

  webbotauth::KeyDirectoryWarmer::Config config;
  config.realm = realm;
  config.host = host;
  config.url = url;
  config.allowlist = ParseWarmAllowlist(allowlist_spec);
  config.refresh_sec = ParseWarmRefreshSec(refresh_spec);
  // The entry TTL must outlast a full refresh cycle (interval + jitter + fetch
  // latency) so keys written this cycle are still valid when the next cycle
  // refreshes them -- otherwise the cache-only path fails closed in the gap.
  // WarmKeyDirectoryCache clamps this to [kTtlMinSec, kTtlMaxSec].
  config.positive_ttl_sec = config.refresh_sec * 2 + 300;
  // Per-worker jitter so workers don't stampede the directory simultaneously.
  config.jitter_sec = static_cast<int64_t>(ngx_pid) % 30;

  std::unique_ptr<webbotauth::KeyDirectoryWarmer> warmer(
      new webbotauth::KeyDirectoryWarmer(config, fetcher, cache,
                                         server_context->thread_system(),
                                         server_context->timer(), handler));
  if (!warmer->Start()) {
    return;  // unique_ptr cleans up; never started so no Join needed
  }
  g_key_directory_warmers.push_back(std::move(warmer));
}

// Called when nginx forks worker processes.  No threads should be started
// before this.
ngx_int_t ps_init_child_process(ngx_cycle_t* cycle) {
  ps_main_conf_t* cfg_m = static_cast<ps_main_conf_t*>(
      ngx_http_cycle_get_module_main_conf(cycle, ngx_pagespeed));
  if (cfg_m == nullptr || cfg_m->driver_factory == nullptr) {
    return NGX_OK;
  }

  // Build-provenance breadcrumb: records the nginx version this
  // module was compiled against, so an operator debugging an ABI-guard
  // pass-through (see ps_report_abi_mismatch) can see at a glance what to
  // rebuild against. Cheap: one NOTICE line per worker at startup.
  ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                "ngx_pagespeed: worker init, built against nginx %s "
                "(runtime ABI guard active)",
                NGINX_VERSION);

  if (!NgxBaseFetch::Initialize(cycle)) {
    return NGX_ERROR;
  }

  // Opt-in counter (experimental): read the SECRET bearer token
  // gating the exact counter document from this worker's environment (populated
  // by nginx's `env` directive). Never logged as a value. The counter file was
  // already mapped by the master (ps_init_module) and inherited across fork.
  ps_webbotauth_counter_read_token();

  // ChildInit() will initialise all ServerContexts, which we need to
  // create ProxyFetchFactories below
  cfg_m->driver_factory->LoggingInit(cycle->log, true);
  cfg_m->driver_factory->ChildInit();

  ngx_http_core_main_conf_t* cmcf = static_cast<ngx_http_core_main_conf_t*>(
      ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module));
  ngx_http_core_srv_conf_t** cscfp =
      static_cast<ngx_http_core_srv_conf_t**>(cmcf->servers.elts);
  ngx_uint_t s;

  // Iterate over all configured server{} blocks, and find our context in it,
  // so we can create and set a ProxyFetchFactory for it.
  std::set<GoogleString> warmer_seen;  // Dedup (host,url) per worker
  bool any_daemon_ready = false;
  for (s = 0; s < cmcf->servers.nelts; s++) {
    ps_srv_conf_t* cfg_s = static_cast<ps_srv_conf_t*>(
        cscfp[s]->ctx->srv_conf[ngx_pagespeed.ctx_index]);
    // Some server{} blocks may not have a ServerContext in that case we must
    // not instantiate a ProxyFetchFactory.
    if (cfg_s->server_context != nullptr) {
      cfg_s->proxy_fetch_factory = new ProxyFetchFactory(cfg_s->server_context);
      ngx_http_core_loc_conf_t* clcf = static_cast<ngx_http_core_loc_conf_t*>(
          cscfp[s]->ctx->loc_conf[ngx_http_core_module.ctx_index]);
      cfg_m->driver_factory->SetServerContextMessageHandler(
          cfg_s->server_context, clcf->error_log);

      // The optimizer-daemon start-up check ran in the master, before the
      // shared message buffer existed; a serving worker repeats a refusal so
      // the admin console's message history shows it.  Not in the cache
      // manager or loader, which run this hook too but serve nothing.
      if ((ngx_process == NGX_PROCESS_WORKER ||
           ngx_process == NGX_PROCESS_SINGLE) &&
          cfg_s->server_context->daemon_adapter() != nullptr) {
        cfg_s->server_context->daemon_adapter()->ReannounceStartupRefusal();
      }

      // Open this worker's own handle on the daemon's shared cache volume
      // for every server whose startup verdict says the daemon is usable.
      // The first open takes a blocking file lock and starts background
      // threads, so it happens here -- in a serving worker, after fork and
      // off the event loop -- never lazily inside a request, and never in
      // the master (the master only resolved the verdict) or in nginx's
      // cache manager and cache loader, which run this hook too but serve
      // no request.  A null result is fine: the adapter has its own retry
      // schedule.
      if ((ngx_process == NGX_PROCESS_WORKER ||
           ngx_process == NGX_PROCESS_SINGLE) &&
          cfg_s->server_context->daemon_health() == DaemonHealth::kReady) {
        cfg_s->server_context->daemon_adapter()->RecordCache();
        any_daemon_ready = true;
      }

      // Start the background key-directory warmer(s) for any server
      // whose feature is enabled AND has a remote directory configured. No-op
      // (default-safe) otherwise. The curl fetcher's poll thread is already up
      // by here (created during ChildInit), which the blocking fetch relies on.
      NgxRewriteOptions* opt = cfg_s->server_context->config();
      if (opt != nullptr && opt->web_bot_auth()) {
        MaybeStartKeyDirectoryWarmer(
            cfg_m->driver_factory, cfg_s->server_context, "wba",
            opt->web_bot_auth_directory_host(),
            opt->web_bot_auth_key_directory_url(),
            opt->web_bot_auth_key_directory_allowlist(),
            opt->web_bot_auth_key_directory_refresh_sec(), &warmer_seen);
      }
      if (opt != nullptr && opt->rsl_cap_enforcement()) {
        MaybeStartKeyDirectoryWarmer(
            cfg_m->driver_factory, cfg_s->server_context, "rsl",
            opt->rsl_cap_directory_host(), opt->rsl_cap_key_directory_url(),
            opt->rsl_cap_key_directory_allowlist(),
            opt->rsl_cap_key_directory_refresh_sec(), &warmer_seen);
      }
    }
  }

  // The record arm's own one-worker pool, created only for a serving
  // process with at least one recordable server (any_daemon_ready is set
  // under exactly that gate above): a commit stalled in the daemon -- a
  // volume lock, a wedged peer -- must never starve an image rewrite on
  // the factory's pools, and the master and nginx's cache manager and
  // loader, which serve nothing, get no pool at all.  ps_exit_child_process
  // shuts it down and deletes it; the sequence is deleted with the pool.
  // The pool's single thread is created lazily, on the event thread, by the
  // first Add -- a completion or an open attempt -- under the pool mutex:
  // bounded, no I/O, once per pool lifetime.
  if (any_daemon_ready) {
    g_daemon_record_pool = new QueuedWorkerPool(
        1, "daemon_record", cfg_m->driver_factory->thread_system());
    g_daemon_record_sequence = g_daemon_record_pool->NewSequence();
  }

  cfg_m->driver_factory->StartThreads();
  return NGX_OK;
}

}  // namespace

}  // namespace net_instaweb

ngx_module_t ngx_pagespeed_etag_filter = {
    NGX_MODULE_V1, &net_instaweb::ps_etag_filter_module,
    nullptr,       NGX_HTTP_MODULE,
    nullptr,       nullptr,
    nullptr,       nullptr,
    nullptr,       nullptr,
    nullptr,       NGX_MODULE_V1_PADDING};

ngx_module_t ngx_pagespeed = {NGX_MODULE_V1,
                              &net_instaweb::ps_module,
                              net_instaweb::ps_commands,
                              NGX_HTTP_MODULE,
                              nullptr,
                              net_instaweb::ps_init_module,
                              net_instaweb::ps_init_child_process,
                              nullptr,
                              nullptr,
                              net_instaweb::ps_exit_child_process,
                              nullptr,
                              NGX_MODULE_V1_PADDING};

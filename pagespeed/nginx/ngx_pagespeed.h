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

#ifndef NGX_PAGESPEED_H_
#define NGX_PAGESPEED_H_

// We might be compiled with syslog.h, which #defines LOG_INFO and LOG_WARNING
// as ints.  But logging.h assumes they're usable as names, within their
// namespace, so we need to #undef them before including logging.h
#ifdef LOG_INFO
#undef LOG_INFO
#endif
#ifdef LOG_WARNING
#undef LOG_WARNING
#endif

extern "C" {
#include <ngx_http.h>
}

#include "base/logging.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

class GzipInflater;
class NgxBaseFetch;
class NgxServerContext;
class ProxyFetch;
class RewriteDriver;
class RequestHeaders;
class ResponseHeaders;
class InPlaceResourceRecorder;
class MappedSharedString;
class Variable;

// Per-drain zero-copy barrier state.  When a
// .pagespeed. resource cache hit is served zero-copy (aliased mmap bytes
// handed into r->out by reference), this pool-scoped struct carries the
// lease pin + the aliased buf so ps_base_fetch_filter can revalidate the
// borrow (intent-checked) and de-alias BEFORE each drain.  Pool-scoped
// (NOT NgxBaseFetch-scoped): a client-paced drain can outlive the base
// fetch, so a pool cleanup owns the pin's lifetime.  buf is the same
// ngx_buf_t linked into r->out, mutated in place on de-alias.
struct PsZeroCopyAlias {
  ngx_buf_t* buf;             // the aliased b->memory=1 buf in r->out
  MappedSharedString* pin;    // heap; released by a request-pool cleanup
  Variable* copied_out_stat;  // nullable
  Variable* renew_fail_stat;  // nullable
  bool done;                  // de-aliased / finalized: barrier is a no-op
};

// Bounded-copy ring for h2/h3 cached serves.  The protocol force-copy
// (see must_copy in NgxBaseFetch::CopyBufferToNginx) stays, but instead of
// one whole-body copy the body is copied out of the pinned mmap region in
// kRingSlotBytes windows through kRingSlotCount recycled slots, bounding
// per-stream worker residency to O(ring) instead of O(body).  2 x 32 KB ~=
// the default h2 stream flow-control window (65535), so a well-paced stream
// always has a full window of bytes in flight between top-filter re-entries
// and never starves.
const size_t kRingSlotBytes = 32 * 1024;
const size_t kRingSlotCount = 2;
// Bodies at or under this keep the single whole-body copy: the per-window
// renew + refill bookkeeping isn't worth four windows or fewer.
const size_t kRingMinBodyBytes = 4 * kRingSlotBytes;

// Bounded-copy ring state, registered as rctx->bounded_ring.  Pool-scoped
// like PsZeroCopyAlias (NOT NgxBaseFetch-scoped, and deliberately a separate
// struct): the drain is client-paced and outlives the base fetch, and the
// request-pool cleanup owning 'pin' is what guarantees a client abort
// mid-stream releases the read lease.
struct PsBoundedCopyRing {
  MappedSharedString* pin;  // heap; released by a request-pool cleanup;
                            // nullptr once every byte left the region
  const char* src;          // borrowed region base (valid while pin is held)
  size_t size;              // borrowed region size in bytes
  size_t cursor;            // bytes copied out of src so far
  ngx_buf_t* slot[kRingSlotCount];  // recycled window bufs (fixed backing)
  size_t next_refill;  // oldest in-flight slot; slots drain in order
  bool done;           // final bytes emitted (or degraded): refill no-op
  // The refill clock.  nginx never re-enters the body-filter chain on its
  // own once downstream has fully consumed the emitted windows: the writer
  // only wakes for flow-control-exhausted resume (r->out backlog), so a
  // fast client whose h2 window covers the in-flight windows would stall
  // until send_timeout without this timer.  1 ms while refills make
  // progress; 25 ms when the in-flight windows have not drained yet (slow
  // clients ride the exhausted-resume writer path, the timer is only their
  // backstop).  Deleted by the pool cleanup on abort.
  ngx_event_t refill_ev;
  Variable* refills_stat;     // nullable
  Variable* copied_out_stat;  // nullable
  Variable* renew_fail_stat;  // nullable
};

// Allocate chain links and buffers from the supplied pool, and copy over the
// data from the string piece.  If the string piece is empty, return
// NGX_DECLINED immediately unless send_last_buf.
ngx_int_t string_piece_to_buffer_chain(ngx_pool_t* pool, StringPiece sp,
                                       ngx_chain_t** link_ptr,
                                       bool send_last_buf, bool send_flush);

StringPiece str_to_string_piece(ngx_str_t s);

// s1: ngx_str_t, s2: string literal
// true if they're equal, false otherwise
#define STR_EQ_LITERAL(s1, s2)     \
  ((s1).len == (sizeof(s2) - 1) && \
   ngx_strncmp((s1).data, (s2), (sizeof(s2) - 1)) == 0)

// s1: ngx_str_t, s2: string literal
// true if they're equal ignoring case, false otherwise
#define STR_CASE_EQ_LITERAL(s1, s2)                                    \
  ((s1).len == (sizeof(s2) - 1) &&                                     \
   ngx_strncasecmp((s1).data,                                          \
                   (reinterpret_cast<u_char*>(const_cast<char*>(s2))), \
                   (sizeof(s2) - 1)) == 0)

// Allocate memory out of the pool for the string piece, and copy the contents
// over.  Returns NULL if we can't get memory.
char* string_piece_to_pool_string(ngx_pool_t* pool, StringPiece sp);

enum PreserveCachingHeaders {
  kPreserveAllCachingHeaders,  // Cache-Control, ETag, Last-Modified, etc
  kPreserveOnlyCacheControl,   // Only Cache-Control.
  kDontPreserveHeaders,
};

typedef struct {
  NgxBaseFetch* base_fetch;

  ngx_http_request_t* r;

  bool html_rewrite;
  bool in_place;

  PreserveCachingHeaders preserve_caching_headers;

  // for html rewrite
  ProxyFetch* proxy_fetch;
  GzipInflater* inflater_;

  // for in place resource
  RewriteDriver* driver;
  InPlaceResourceRecorder* recorder;
  ResponseHeaders* ipro_response_headers;

  // We need to remember the URL here as well since we may modify what NGX
  // gets by stripping our special query params and honoring X-Forwarded-Proto.
  GoogleString url_string;

  // We need to remember if the upstream had headers_out->location set, because
  // we should mirror that when we write it back. nginx may absolutify
  // Location: headers that start with '/' without regarding X-Forwarded-Proto.
  bool location_field_set;
  bool psol_vary_accept_only;
  bool follow_flushes;

  // Per-drain zero-copy barrier (nullptr unless this request is
  // serving a .pagespeed. cache hit zero-copy).  Set by NgxBaseFetch at
  // emit; consulted by ps_base_fetch_filter before every drain.
  PsZeroCopyAlias* zerocopy_alias;

  // Bounded-copy ring (nullptr unless this request is an h2/h3 cached
  // serve running the ring).  Set by NgxBaseFetch at emit; refilled by
  // ps_base_fetch_filter on every invocation while incomplete.  Mutually
  // exclusive with zerocopy_alias.
  PsBoundedCopyRing* bounded_ring;
} ps_request_ctx_t;

ps_request_ctx_t* ps_get_request_context(ngx_http_request_t* r);

void copy_request_headers_from_ngx(const ngx_http_request_t* r,
                                   RequestHeaders* headers);

void copy_response_headers_from_ngx(const ngx_http_request_t* r,
                                    ResponseHeaders* headers);

ngx_int_t copy_response_headers_to_ngx(
    ngx_http_request_t* r, const ResponseHeaders& pagespeed_headers,
    PreserveCachingHeaders preserve_caching_headers);

StringPiece ps_determine_host(ngx_http_request_t* r);

// Returns the NgxServerContext for this request, or nullptr if pagespeed is not
// active for it. Thin accessor so other translation units (e.g. the Web-Bot-Auth
// glue) can reach the server context without seeing the ps_srv_conf_t internals.
NgxServerContext* ps_get_server_context(ngx_http_request_t* r);

namespace ps_base_fetch {

ngx_int_t ps_base_fetch_handler(ngx_http_request_t* r);

// Bounded-copy ring: copies the next window out of the pinned region
// into the oldest drained slot (or, under wrap pressure, the whole remaining
// tail into a one-shot pool buffer) and returns exactly one chain link in
// *out.  Marks ring->done and releases the pin once the emitted link carries
// the final body bytes.  Returns NGX_ERROR when the borrow verified torn
// after the copy (the stream must be reset) or on allocation failure.
ngx_int_t ps_bounded_ring_emit_next(ngx_http_request_t* r,
                                    PsBoundedCopyRing* ring, ngx_chain_t** out);

// Refill-clock timer handler (PsBoundedCopyRing::refill_ev).
void ps_bounded_ring_refill_event(ngx_event_t* ev);

}  // namespace ps_base_fetch

}  // namespace net_instaweb

#endif  // NGX_PAGESPEED_H_
